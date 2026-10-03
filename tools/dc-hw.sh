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
#       -u, --until REGEX  stop as soon as the output matches REGEX
#       -l, --log FILE     also write the program's output to FILE
#
# "run" brings the console up if it is off or not answering, uploads the
# program and prints what it prints. It exits with 0 if --until matched, 124
# on timeout, otherwise with dc-tool-ip's own exit status.
#
# Programs that use the network (INIT_NET) take the adapter over from
# dcload-ip, and dc-tool-ip does not always notice when they exit. Give those
# an --until pattern for their last line, and --cycle the next run so that it
# starts from a freshly booted dcload-ip.
#
# Settings, overridable from the environment:
#   DC_IP         Dreamcast address            (192.168.1.150)
#   DC_PLUG_IP    Shelly plug address          (192.168.1.173)
#   DC_TOOL       path to dc-tool-ip
#   DC_BOOT_WAIT  seconds to allow for booting (120)
#   DC_OFF_SECS   seconds to stay off in cycle (2)

set -u

DC_IP="${DC_IP:-192.168.1.150}"
DC_PLUG_IP="${DC_PLUG_IP:-192.168.1.173}"
DC_TOOL="${DC_TOOL:-$HOME/Dropbox/Games/ROMs/DREAMCAST/dcload-ip/dc-tool-ip}"
DC_BOOT_WAIT="${DC_BOOT_WAIT:-120}"
DC_OFF_SECS="${DC_OFF_SECS:-2}"

say() { printf '[dc-hw %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die() { say "error: $*"; exit 1; }

plug_rpc() {
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

# dcload-ip answers pings; the BIOS and openMenu do not.
dc_up() { ping -c 1 -t 1 "$DC_IP" >/dev/null 2>&1; }

dc_wait() {
    local limit="${1:-$DC_BOOT_WAIT}" start=$SECONDS

    say "waiting up to ${limit}s for dcload-ip at $DC_IP"

    while ! dc_up; do
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
    if dc_up; then
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
    local cycle=0 timeout=300 until="" log="" elf=""

    while (( $# )); do
        case "$1" in
            -c|--cycle)   cycle=1 ;;
            -T|--timeout) timeout="$2"; shift ;;
            -u|--until)   until="$2"; shift ;;
            -l|--log)     log="$2"; shift ;;
            --)           shift; break ;;
            -*)           die "unknown option $1" ;;
            *)            elf="$1"; shift; [[ "${1:-}" == "--" ]] && shift; break ;;
        esac
        shift
    done

    [[ -n "$elf" ]] || die "usage: dc-hw.sh run [-c] [-T secs] [-u regex] [-l log] prog.elf"
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
    elif ! dc_up; then
        say "console is on but dcload-ip is not answering"
        power_cycle
        dc_wait || return 1
    fi

    local out="$log" keep=1
    if [[ -z "$out" ]]; then
        out="$(mktemp -t dc-hw)"
        keep=0
    fi

    say "running $(basename "$elf") (timeout ${timeout}s)"

    # dc-tool-ip stays attached to serve the program's console, so run it in
    # the background, echo what it writes, and watch the clock.
    local status=0 start=$SECONDS pid shown=0 size

    "$DC_TOOL" -t "$DC_IP" -q -x "$elf" "$@" </dev/null >"$out" 2>&1 &
    pid=$!

    show_new() {
        size=$(wc -c <"$out")
        if (( size > shown )); then
            tail -c "+$(( shown + 1 ))" "$out" | head -c "$(( size - shown ))"
            shown=$size
        fi
    }

    while kill -0 "$pid" 2>/dev/null; do
        show_new

        if [[ -n "$until" ]] && grep -Eq -- "$until" "$out"; then
            say "output matched /$until/"
            status=-1
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

    show_new
    (( keep )) || rm -f "$out"

    say "finished in $(( SECONDS - start ))s, status $status"
    return "$status"
}

case "${1:-}" in
    status) cmd_status ;;
    power)  shift; cmd_power "$@" ;;
    wait)   shift; dc_wait "$@" ;;
    reset)  "$DC_TOOL" -t "$DC_IP" -r ;;
    run)    shift; cmd_run "$@" ;;
    *)      sed -n '3,29p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
