#!/usr/bin/env bash
#
# dc-hw.sh - run programs on the real Dreamcast from this Mac.
#
# The console boots from GDEMU into openMenu, which auto-launches dcload-ip.
# Its power goes through a Shelly smart plug, so a hung or crashed console can
# be recovered without touching it.
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
#
# "run" brings the console up if it is off or not answering, uploads the
# program and prints what it prints. If the program prints nothing and
# dcload-ip is back waiting, the upload is retried, the last time after a
# power cycle. It exits with 0 if
# --until matched, 124 on timeout or --idle, 125 if the program never started,
# otherwise with dc-tool-ip's own exit status.
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
#   DC_IP         Dreamcast address            (last one found, else 192.168.1.74)
#   DC_PLUG_IP    Shelly plug address          (last one found, else 192.168.1.173;
#                 if it stops answering, the /24 is scanned for a Shelly plug)
#   DC_TOOL       path to dc-tool-ip
#   DC_BOOT_WAIT  seconds to allow for booting (120)
#   DC_OFF_SECS   seconds to stay off in cycle (2)
#   DC_START_SECS seconds of silence after upload before retrying it (15)

set -u

DC_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/dc-hw"
DC_IP="${DC_IP:-$(cat "$DC_CACHE/ip" 2>/dev/null || echo 192.168.1.74)}"
DC_PLUG_IP="${DC_PLUG_IP:-$(cat "$DC_CACHE/plug_ip" 2>/dev/null || echo 192.168.1.173)}"
DC_TOOL="${DC_TOOL:-$HOME/Dropbox/Games/ROMs/DREAMCAST/dcload-ip/dc-tool-ip}"
DC_BOOT_WAIT="${DC_BOOT_WAIT:-120}"
DC_OFF_SECS="${DC_OFF_SECS:-2}"
DC_START_SECS="${DC_START_SECS:-15}"

say() { printf '[dc-hw %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die() { say "error: $*"; exit 1; }

# Shelly devices describe themselves at /shelly; find a plug on the /24 if
# DHCP moved it, and remember where it answered.
find_plug() {
    local net="${DC_PLUG_IP%.*}" i dir found
    dir="$(mktemp -d -t dc-hw-plug)"
    for i in $(seq 1 254); do
        { curl -fsS -m 2 "http://$net.$i/shelly" 2>/dev/null |
              grep -q '"app":"Plug' && touch "$dir/$net.$i"; } &
    done
    wait
    found="$(ls "$dir" | head -n 1)"
    rm -rf "$dir"
    [[ -n "$found" ]] || return 1
    say "Shelly plug found at $found (was $DC_PLUG_IP)"
    DC_PLUG_IP="$found"
    mkdir -p "$DC_CACHE" && echo "$DC_PLUG_IP" >"$DC_CACHE/plug_ip"
}

plug_rpc() {
    curl -fs -m 5 "http://$DC_PLUG_IP/rpc/$1" && return
    say "the plug at $DC_PLUG_IP did not answer; searching the network"
    find_plug || die "no Shelly plug answered on ${DC_PLUG_IP%.*}.0/24"
    curl -fsS -m 5 "http://$DC_PLUG_IP/rpc/$1" ||
        die "the plug at $DC_PLUG_IP did not answer"
}

plug_set() { plug_rpc "Switch.Set?id=0&on=$1" >/dev/null; }

# Prints "on" or "off".
plug_state() {
    plug_rpc "Switch.GetStatus?id=0" |
        sed -En 's/.*"output":[[:space:]]*(true|false).*/\1/p' |
        sed 's/true/on/; s/false/off/'
}

plug_watts() {
    plug_rpc "Switch.GetStatus?id=0" |
        sed -n 's/.*"apower":[[:space:]]*\([0-9.]*\).*/\1/p'
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
    local state
    state="$(plug_state)"
    echo "plug:      $state, $(plug_watts) W"
    if dc_up find; then
        echo "dcload-ip: answering at $DC_IP"
    else
        echo "dcload-ip: not answering at $DC_IP"
    fi
}

cmd_power() {
    case "${1:-}" in
        on)    plug_set true;  say "power on" ;;
        off)   plug_set false; say "power off" ;;
        cycle) power_cycle ;;
        *)     die "usage: dc-hw.sh power on|off|cycle" ;;
    esac
}

cmd_run() {
    local cycle=0 timeout=300 idle=0 until="" log="" elf=""

    while (( $# )); do
        case "$1" in
            -c|--cycle)   cycle=1 ;;
            -T|--timeout) timeout="$2"; shift ;;
            -i|--idle)    idle="$2"; shift ;;
            -u|--until)   until="$2"; shift ;;
            -l|--log)     log="$2"; shift ;;
            --)           shift; break ;;
            -*)           die "unknown option $1" ;;
            *)            elf="$1"; shift; [[ "${1:-}" == "--" ]] && shift; break ;;
        esac
        shift
    done

    [[ -n "$elf" ]] || die "usage: dc-hw.sh run [-c] [-T secs] [-i secs] [-u regex] [-l log] prog.elf"
    [[ -f "$elf" ]] || die "$elf does not exist"
    [[ -x "$DC_TOOL" ]] || die "dc-tool-ip not found at $DC_TOOL"

    if (( cycle )); then
        power_cycle
        dc_wait || return 1
    elif [[ "$(plug_state)" != on ]]; then
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
    local status start pid shown size attempt started quiet_since

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
            say "rebooting the console for a last try"
            power_cycle
            dc_wait || return 1
        fi
        say "running $(basename "$elf") (timeout ${timeout}s)"
        status=0 start=$SECONDS shown=0 started=0 quiet_since=$SECONDS

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
            # reset) means the program is not running.
            if (( ! started && SECONDS - start >= DC_START_SECS )) && dc_up find; then
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
    return "$status"
}

case "${1:-}" in
    status) cmd_status ;;
    power)  shift; cmd_power "$@" ;;
    wait)   shift; dc_wait "$@" ;;
    reset)  dc_up find; "$DC_TOOL" -t "$DC_IP" -r ;;
    run)    shift; cmd_run "$@" ;;
    *)      sed -n '3,/^$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
