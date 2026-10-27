#!/usr/bin/env bash
#
# dc-hw.sh - run programs on a real Dreamcast over dcload-ip.
#
# The console needs a Broadband Adapter and dcload-ip (mine boots from GDEMU
# into openMenu, which auto-launches dcload-ip). If its power goes through a
# Shelly smart plug, a hung or crashed console can be recovered without
# touching it, but the plug is only used once you name it:
#
#   dc-hw.sh --use-shelly [IP] ...  use the Shelly plug at IP (or the only one
#                                   on the network) and remember it
#   dc-hw.sh --no-shelly ...        forget it; switch the console yourself
#
# The plug's MAC address is remembered with its address, so a plug that DHCP
# moved is found again and never mixed up with another one on the network.
#
#   dc-hw.sh status                 plug state, power draw, is dcload answering
#   dc-hw.sh power on|off|cycle     switch the plug (cycle = off, pause, on)
#   dc-hw.sh wait [seconds]         wait until dcload-ip answers on the network
#   dc-hw.sh reset                  ask dcload-ip to reset the console
#   dc-hw.sh run [options] prog.elf [-- extra dc-tool-ip options]
#       -c, --cycle        power-cycle and wait for dcload-ip first
#       -T, --timeout N    give up after N seconds (default 300)
#       -i, --idle N       give up if the program prints nothing for N seconds
#       -u, --until REGEX  stop as soon as the output matches REGEX
#       -l, --log FILE     also write the program's output to FILE
#       -o, --off          switch the console off when the run ends (scripted
#                          tests and benchmarks; leave it off for play sessions)
#
# "run" brings the console up if it is off or not answering, uploads the
# program and prints what it prints. If the program prints nothing and
# dcload-ip is back waiting, the upload is retried, the last time after a
# power cycle. Without a plug, switch the console on into dcload-ip first:
# --cycle then runs on the dcload-ip that is up, and --off only reminds you to
# switch it off. It exits with 0 if
# --until matched, 124 on timeout or --idle, 125 if the program never started,
# otherwise with dc-tool-ip's own exit status.
#
# Switch the console off when a test is done: use --off for scripted runs, or
# `dc-hw.sh power off` afterwards. Without --off, run ends with a reminder
# that the console is still on.
#
# Programs that use the network (INIT_NET) take the adapter over from
# dcload-ip, and dc-tool-ip does not always notice when they exit. Give those
# an --until pattern for their last line, and --cycle the next run so that it
# starts from a freshly booted dcload-ip.
#
# dcload-ip takes its address from DHCP, so it can change between boots. The
# console is found by asking for dcload-ip's version on UDP port 31313: the
# last address that answered is tried first, then the rest of its /24. (Its
# MAC address is no help: the adapter sits behind a bridge that rewrites it.)
#
# Settings, overridable from the environment:
#   DC_IP         Dreamcast address            (last one found, else this
#                 host's address, whose /24 is searched)
#   DC_PLUG_IP    Shelly plug address, the same as --use-shelly IP
#   DC_TOOL       path to dc-tool-ip           (the one on PATH)
#   DC_BOOT_WAIT  seconds to allow for booting (120)
#   DC_OFF_SECS   seconds to stay off in cycle (2)
#   DC_START_SECS seconds of silence after the execute command before
#                 uploading again (15)
#   DC_UPLOAD_SECS seconds to allow for the upload itself (120)

set -u

# This host's IPv4 address, to guess which /24 the console is on.
host_ip() {
    local ifc
    if command -v ipconfig >/dev/null 2>&1; then
        ifc="$(route -n get default 2>/dev/null | awk '/interface:/ {print $2}')"
        ipconfig getifaddr "${ifc:-en0}" 2>/dev/null
    else
        ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p'
    fi
}

DC_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/dc-hw"
DC_IP="${DC_IP:-$(cat "$DC_CACHE/ip" 2>/dev/null || host_ip)}"
DC_TOOL="${DC_TOOL:-$(command -v dc-tool-ip || echo "$HOME/Dropbox/Games/ROMs/DREAMCAST/dcload-ip/dc-tool-ip")}"
DC_BOOT_WAIT="${DC_BOOT_WAIT:-120}"
DC_OFF_SECS="${DC_OFF_SECS:-2}"
DC_START_SECS="${DC_START_SECS:-15}"
DC_UPLOAD_SECS="${DC_UPLOAD_SECS:-120}"

say() { printf '[dc-hw %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die() { say "error: $*"; exit 1; }

# The console's Shelly plug, if there is one (see --use-shelly).
PLUG_IP="$(cat "$DC_CACHE/plug_ip" 2>/dev/null)"
PLUG_MAC="$(cat "$DC_CACHE/plug_mac" 2>/dev/null)"

remember_plug() {
    mkdir -p "$DC_CACHE"
    echo "$PLUG_IP" >"$DC_CACHE/plug_ip"
    echo "$PLUG_MAC" >"$DC_CACHE/plug_mac"
}

# Shelly devices describe themselves at /shelly. Prints the MAC address of
# the one at $1.
shelly_mac() {
    curl -fs -m 3 "http://$1/shelly" 2>/dev/null |
        sed -n 's/.*"mac":[[:space:]]*"\([0-9A-Fa-f]*\)".*/\1/p'
}

# Every Shelly device on the /24 of $1, one "address mac app" line each.
shelly_scan() {
    local net="${1%.*}" i dir f mac
    dir="$(mktemp -d -t dc-hw-plug)"
    for i in $(seq 1 254); do
        curl -fs -m 2 -o "$dir/$net.$i" "http://$net.$i/shelly" 2>/dev/null &
    done
    wait
    for f in "$dir"/*; do
        [[ -s "$f" ]] || continue
        mac="$(sed -n 's/.*"mac":[[:space:]]*"\([0-9A-Fa-f]*\)".*/\1/p' "$f")"
        [[ -n "$mac" ]] || continue
        echo "$(basename "$f") $mac $(sed -n 's/.*"app":[[:space:]]*"\([^"]*\)".*/\1/p' "$f")"
    done
    rm -rf "$dir"
}

# Use the Shelly plug at $1 from now on.
use_plug() {
    local mac
    mac="$(shelly_mac "$1")"
    [[ -n "$mac" ]] || die "no Shelly device answered at $1"
    curl -fs -m 5 "http://$1/rpc/Switch.GetStatus?id=0" >/dev/null ||
        die "the Shelly device at $1 has no switch to power the console with"
    if [[ "$1" != "$PLUG_IP" || "$mac" != "$PLUG_MAC" ]]; then
        say "using the Shelly plug at $1 ($mac) from now on; --no-shelly forgets it"
    fi
    PLUG_IP="$1" PLUG_MAC="$mac"
    remember_plug
}

# A bare --use-shelly takes the only plug on the console's network. With
# several there is no telling which one the console is on, so ask.
pick_plug() {
    local plugs
    [[ -n "$DC_IP" ]] || die "can't tell which network to search: give the plug's address, --use-shelly IP"
    plugs="$(shelly_scan "$DC_IP" | awk '$3 ~ /Plug/ {print $1}')"
    case "$(printf '%s' "$plugs" | grep -c .)" in
        0) die "no Shelly plug answered on ${DC_IP%.*}.0/24; give its address: --use-shelly IP" ;;
        1) use_plug "$plugs" ;;
        *) die "several Shelly plugs answered on ${DC_IP%.*}.0/24 (${plugs//$'\n'/ }); pick the console's: --use-shelly IP" ;;
    esac
}

# DHCP can move the plug. Look for it by MAC address, so that another plug on
# the network is never switched in its place.
find_plug() {
    local ip mac app
    [[ -n "$PLUG_MAC" ]] || return 1
    while read -r ip mac app; do
        [[ "$mac" == "$PLUG_MAC" ]] || continue
        say "Shelly plug $PLUG_MAC found at $ip (was $PLUG_IP)"
        PLUG_IP="$ip"
        remember_plug
        return 0
    done < <(shelly_scan "$PLUG_IP")
    return 1
}

# Calls the plug's RPC method $1 and leaves the reply in PLUG_OUT, in this
# shell rather than a subshell, so a plug found at a new address stays found.
plug_rpc() {
    if PLUG_OUT="$(curl -fs -m 5 "http://$PLUG_IP/rpc/$1")"; then
        # A plug remembered by an older dc-hw.sh has no MAC address yet.
        if [[ -z "$PLUG_MAC" ]]; then
            PLUG_MAC="$(shelly_mac "$PLUG_IP")"
            remember_plug
        fi
        return 0
    fi
    say "the plug at $PLUG_IP did not answer; looking for it on the network"
    find_plug || die "Shelly plug ${PLUG_MAC:-at $PLUG_IP} is not on ${PLUG_IP%.*}.0/24 (is it plugged in? --use-shelly IP points at it again)"
    PLUG_OUT="$(curl -fsS -m 5 "http://$PLUG_IP/rpc/$1")" ||
        die "the plug at $PLUG_IP did not answer"
}

plug_set() { plug_rpc "Switch.Set?id=0&on=$1"; }

# Reads the switch into PLUG_ON ("on" or "off") and the draw into PLUG_WATTS.
plug_query() {
    plug_rpc "Switch.GetStatus?id=0"
    PLUG_ON="$(sed -En 's/.*"output":[[:space:]]*(true|false).*/\1/p' <<<"$PLUG_OUT" |
        sed 's/true/on/; s/false/off/')"
    PLUG_WATTS="$(sed -n 's/.*"apower":[[:space:]]*\([0-9.]*\).*/\1/p' <<<"$PLUG_OUT")"
}

no_plug_help() {
    cat >&2 <<'EOF'
[dc-hw] There is no Shelly plug set up, so the console can't be switched from here.
  - Switch the console on into dcload-ip yourself, then run this again
  - Or, if the console is on a Shelly plug: dc-hw.sh --use-shelly [IP] (remembered)
  - Or skip dc-hw.sh: build with make, then load the ELF your usual way, e.g.
      dc-tool-ip -t <console-ip> -x prog.elf
    or make a CDI for GDEMU or a CD-R: tools/flycast/elf-to-cdi.sh prog.elf prog.cdi
EOF
}

remember_ip() {
    [[ "$1" == "$DC_IP" ]] && return
    say "dcload-ip found at $1 (was $DC_IP)"
    DC_IP="$1"
    mkdir -p "$DC_CACHE" && echo "$DC_IP" >"$DC_CACHE/ip"
}

# Does dcload-ip answer a version request at this address? The BIOS,
# openMenu and running programs do not.
dcload_at() {
    printf 'VERS\0\0\0\0\0\0\0\0' | nc -u -w 1 "$1" 31313 2>/dev/null |
        tr -d '\0' | grep -q dcload
}

# Ask every address in DC_IP's /24 at once; print the first to answer.
sweep() {
    local net="${DC_IP%.*}" i dir
    dir="$(mktemp -d -t dc-hw)"
    for i in $(seq 1 254); do
        { dcload_at "$net.$i" && touch "$dir/$net.$i"; } &
    done
    wait
    ls "$dir" | head -n 1
    rm -rf "$dir"
}

# Is dcload-ip answering? With "find", search the network when it is not at
# the last known address.
dc_up() {
    local ip
    [[ -n "$DC_IP" ]] || die "can't tell which network the console is on: set DC_IP"
    dcload_at "$DC_IP" && return 0
    [[ "${1:-}" == find ]] || return 1
    ip="$(sweep)"
    [[ -n "$ip" ]] || return 1
    remember_ip "$ip"
}

dc_wait() {
    local limit="${1:-$DC_BOOT_WAIT}" start=$SECONDS

    say "waiting up to ${limit}s for dcload-ip (last seen at $DC_IP)"

    local tries=0 how
    while :; do
        # Search the whole network every fifth try.
        how=""
        (( ++tries % 5 == 0 )) && how=find
        dc_up "$how" && break
        if (( SECONDS - start >= limit )); then
            say "no answer after ${limit}s"
            return 1
        fi
        sleep 1
    done

    # Let it finish settling before the first upload.
    sleep 2
    say "dcload-ip is up after $(( SECONDS - start ))s"
}

power_cycle() {
    say "power off"
    plug_set false
    sleep "$DC_OFF_SECS"
    say "power on"
    plug_set true
}

cmd_status() {
    if [[ -n "$PLUG_IP" ]]; then
        plug_query
        echo "plug:      $PLUG_ON, $PLUG_WATTS W (Shelly at $PLUG_IP)"
    else
        echo "plug:      none (dc-hw.sh --use-shelly [IP] to use one)"
    fi
    if dc_up find; then
        echo "dcload-ip: answering at $DC_IP"
    else
        echo "dcload-ip: not answering at $DC_IP"
    fi
}

cmd_power() {
    [[ -n "$PLUG_IP" ]] || { no_plug_help; exit 1; }
    case "${1:-}" in
        on)    plug_set true;  say "power on" ;;
        off)   plug_set false; say "power off" ;;
        cycle) power_cycle ;;
        *)     die "usage: dc-hw.sh power on|off|cycle" ;;
    esac
}

cmd_run() {
    local cycle=0 timeout=300 idle=0 until="" log="" elf="" off=0

    while (( $# )); do
        case "$1" in
            -c|--cycle)   cycle=1 ;;
            -T|--timeout) timeout="$2"; shift ;;
            -i|--idle)    idle="$2"; shift ;;
            -u|--until)   until="$2"; shift ;;
            -l|--log)     log="$2"; shift ;;
            -o|--off)     off=1 ;;
            --)           shift; break ;;
            -*)           die "unknown option $1" ;;
            *)            elf="$1"; shift; [[ "${1:-}" == "--" ]] && shift; break ;;
        esac
        shift
    done

    [[ -n "$elf" ]] || die "usage: dc-hw.sh run [-c] [-o] [-T secs] [-i secs] [-u regex] [-l log] prog.elf"
    [[ -f "$elf" ]] || die "$elf does not exist"
    [[ -x "$DC_TOOL" ]] || die "dc-tool-ip not found (put it on your PATH or set DC_TOOL)"

    if [[ -z "$PLUG_IP" ]]; then
        if ! dc_up find; then
            say "dcload-ip is not answering; switch the console on into dcload-ip"
            dc_wait || { no_plug_help; return 1; }
        elif (( cycle )); then
            say "no Shelly plug to power-cycle with: using the dcload-ip that is up"
        fi
    elif (( cycle )); then
        power_cycle
        dc_wait || return 1
    elif plug_query; [[ "$PLUG_ON" != on ]]; then
        say "console is off"
        plug_set true
        say "power on"
        dc_wait || return 1
    elif ! dc_up find; then
        say "console is on but dcload-ip is not answering"
        power_cycle
        dc_wait || return 1
    fi

    local out="$log" keep=1
    if [[ -z "$out" ]]; then
        out="$(mktemp -t dc-hw)"
        keep=0
    fi

    # dc-tool-ip stays attached to serve the program's console, so run it in
    # the background, echo what it writes, and watch the clock.
    local status start pid shown size attempt started quiet_since exec_at

    show_new() {
        size=$(wc -c <"$out")
        if (( size > shown )); then
            tail -c "+$(( shown + 1 ))" "$out" | head -c "$(( size - shown ))"
            shown=$size
        fi
    }

    # Now and then the console takes the upload and the execute command but
    # drops straight back into dcload-ip without running anything. Notice
    # the silence and upload again.
    for attempt in 1 2 3; do
        if (( attempt == 3 )); then
            if [[ -z "$PLUG_IP" ]]; then
                say "reboot the console into dcload-ip and run this again"
                break
            fi
            say "rebooting the console for a last try"
            power_cycle
            dc_wait || return 1
        fi
        say "running $(basename "$elf") (timeout ${timeout}s)"
        status=0 start=$SECONDS shown=0 started=0 quiet_since=$SECONDS exec_at=0

        "$DC_TOOL" -t "$DC_IP" -q -x "$elf" "$@" </dev/null >"$out" 2>&1 &
        pid=$!

        while kill -0 "$pid" 2>/dev/null; do
            size=$(wc -c <"$out")
            (( size > shown )) && quiet_since=$SECONDS
            show_new

            if (( ! started )) && sed '1,/executing/d' "$out" | grep -q .; then
                started=1
            fi

            # dcload-ip answering again (perhaps at a new address after a
            # reset) means the program is not running. It also answers
            # while it is still receiving the upload, which takes a while
            # for a big ELF, so count from the execute command.
            if (( ! exec_at )) && grep -q executing "$out"; then
                exec_at=$SECONDS
            fi
            if (( ! started )) && { (( exec_at && SECONDS - exec_at >= DC_START_SECS )) ||
                                    (( SECONDS - start >= DC_UPLOAD_SECS )); } && dc_up find; then
                say "the program did not start; dcload-ip is waiting at $DC_IP"
                status=125
                break
            fi

            if [[ -n "$until" ]] && grep -Eq -- "$until" "$out"; then
                say "output matched /$until/"
                status=-1
                break
            fi

            if (( idle && started && SECONDS - quiet_since >= idle )); then
                say "no output for ${idle}s"
                status=124
                break
            fi

            if (( SECONDS - start >= timeout )); then
                say "timed out after ${timeout}s"
                status=124
                break
            fi

            sleep 1
        done

        if (( status == 0 )); then
            wait "$pid"
            status=$?
        else
            kill "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            (( status == -1 )) && status=0
        fi

        (( status == 125 )) || break
    done
    show_new
    (( keep )) || rm -f "$out"

    say "finished in $(( SECONDS - start ))s, status $status"
    if [[ -z "$PLUG_IP" ]]; then
        (( off )) && say "no Shelly plug: switch the console off yourself"
    elif (( off )); then
        plug_set false
        say "power off"
    else
        say "the console is still on: 'dc-hw.sh power off' when you are done with it"
    fi
    return "$status"
}

shelly=""
while (( $# )); do
    case "$1" in
        --use-shelly)
            shelly=auto
            if [[ "${2:-}" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                shelly="$2"
                shift
            fi ;;
        --use-shelly=*) shelly="${1#*=}" ;;
        --no-shelly)    shelly=none ;;
        *)              break ;;
    esac
    shift
done
[[ -z "$shelly" ]] && shelly="${DC_PLUG_IP:-}"

case "$shelly" in
    "")   ;;
    none) [[ -n "$PLUG_IP" ]] && say "forgot the Shelly plug at $PLUG_IP"
          rm -f "$DC_CACHE/plug_ip" "$DC_CACHE/plug_mac"
          PLUG_IP="" PLUG_MAC="" ;;
    auto) [[ -n "$PLUG_IP" ]] || pick_plug ;;
    *)    use_plug "$shelly" ;;
esac
[[ -z "${1:-}" && -n "$shelly" ]] && exit 0

case "${1:-}" in
    status) cmd_status ;;
    power)  shift; cmd_power "$@" ;;
    wait)   shift; dc_wait "$@" ;;
    reset)  dc_up find; "$DC_TOOL" -t "$DC_IP" -r ;;
    run)    shift; cmd_run "$@" ;;
    *)      sed -n '3,/^$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
