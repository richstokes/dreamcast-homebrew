#!/usr/bin/env bash
#
# dc-dhcp-test.sh - power-cycle the Dreamcast and check dcload-ip's DHCP handshake.
#
# Records DHCP traffic on this Mac while the console boots from GDEMU through
# openMenu into dcload-ip (power and waiting are done by dc-hw.sh), keeps
# listening for a while after it is up, and then checks what the console sent
# and what the server gave it:
#
#   - every packet from the console came from 0.0.0.0 with the hostname set
#   - each DISCOVER used a fresh transaction ID
#   - one handshake only: the server's replies all belong to one transaction,
#     and there is no new OFFER or DISCOVER after the console was ACKed
#   - the address the server ACKed is the one dcload-ip answers on
#   - nothing more from the console once it was bound
#
#   dc-dhcp-test.sh [-i iface] [-w secs] [-o capture.txt] [-n]
#   dc-dhcp-test.sh -f capture.txt [-m mac] [-a ip]
#       -i IFACE    interface to listen on (default: the default route's, usually en0)
#       -w SECS     keep listening this long after dcload-ip is up (default 30)
#       -o FILE     where to keep the tcpdump text (default ~/.cache/dc-hw/dhcp-<time>.txt)
#       -n          don't power-cycle: ask dcload-ip to reset itself instead (dc-hw.sh reset)
#       -f FILE     no capture or power cycle, just check an earlier capture
#       -m MAC      the console's MAC for -f (default: ARP entry of its address)
#       -a IP       the console's address for -f (default: the one dc-hw.sh last saw)
#
# tcpdump needs sudo, so run this from a terminal where you can type your
# password. Exits 0 when every check passes, 1 otherwise, 2 if the console
# never came up.
#
# A Mac on Wi-Fi only sees the console's broadcasts when the access point
# forwards them, and it often doesn't; the server's replies always get
# through. When none of the console's own packets were captured, the checks
# on them are skipped and the handshake is judged from the replies alone. A
# wired Mac on the same switch sees everything.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DC_HW="$HERE/dc-hw.sh"
DC_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/dc-hw"

IFACE="$(route -n get default 2>/dev/null | awk '/interface:/ {print $2}')"
IFACE="${IFACE:-en0}"
WATCH=30
CAPTURE=""
RESET_ONLY=0
CHECK_ONLY=""
DC_MAC="${DC_MAC:-}"
DC_IP="${DC_IP:-}"

say() { printf '[dc-dhcp %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die() { say "error: $*"; exit 2; }

while getopts "i:w:o:nf:m:a:h" opt; do
    case "$opt" in
        i) IFACE="$OPTARG" ;;
        w) WATCH="$OPTARG" ;;
        o) CAPTURE="$OPTARG" ;;
        n) RESET_ONLY=1 ;;
        f) CHECK_ONLY="$OPTARG" ;;
        m) DC_MAC="$OPTARG" ;;
        a) DC_IP="$OPTARG" ;;
        *) sed -n '3,/^$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
    esac
done

[[ -x "$DC_HW" ]] || die "dc-hw.sh not found next to this script"

# macOS prints MAC octets without leading zeros; tcpdump pads them.
mac_of() {
    ping -c 1 -W 1000 "$1" >/dev/null 2>&1
    arp -n "$1" | sed -n 's/.* at \([0-9a-f:]*\) .*/\1/p' |
        awk -F: '{ for (i = 1; i <= NF; i++) printf "%s%02s", (i > 1 ? ":" : ""), $i }' | tr ' ' 0
}

if [[ -z "$CHECK_ONLY" ]]; then
    mkdir -p "$DC_CACHE"
    [[ -n "$CAPTURE" ]] || CAPTURE="$DC_CACHE/dhcp-$(date +%Y%m%d-%H%M%S).txt"

    if networksetup -listallhardwareports 2>/dev/null | grep -B1 "Device: $IFACE\$" | grep -q Wi-Fi; then
        say "note: $IFACE is Wi-Fi, so the console's own packets may be missing from the capture"
    fi

    sudo -v || die "tcpdump needs sudo"

    say "listening for DHCP on $IFACE, capture in $CAPTURE"
    sudo tcpdump -i "$IFACE" -n -vvv -l 'port 67 or port 68' >"$CAPTURE" 2>/dev/null &
    TCPDUMP_PID=$!
    cleanup() { sudo kill "$TCPDUMP_PID" 2>/dev/null; wait "$TCPDUMP_PID" 2>/dev/null; }
    trap cleanup EXIT
    sleep 1
    kill -0 "$TCPDUMP_PID" 2>/dev/null || die "tcpdump did not start"

    if (( RESET_ONLY )); then
        "$DC_HW" reset || die "dcload-ip did not take the reset"
    else
        "$DC_HW" power cycle || exit 2
    fi
    "$DC_HW" wait || exit 2

    DC_IP="$("$DC_HW" status 2>/dev/null | sed -n 's/.*answering at \([0-9.]*\).*/\1/p')"
    [[ -n "$DC_IP" ]] || die "dc-hw.sh does not see dcload-ip"
    DC_MAC="$(mac_of "$DC_IP")"
    [[ -n "$DC_MAC" ]] || die "no ARP entry for $DC_IP"
    say "dcload-ip is at $DC_IP ($DC_MAC); listening ${WATCH}s more for stray traffic"
    sleep "$WATCH"

    cleanup
    trap - EXIT
else
    CAPTURE="$CHECK_ONLY"
    [[ -f "$CAPTURE" ]] || die "$CAPTURE does not exist"
    [[ -n "$DC_IP" ]] || DC_IP="$(cat "$DC_CACHE/ip" 2>/dev/null)"
    [[ -n "$DC_IP" ]] || die "which address is the console on? pass -a"
    [[ -n "$DC_MAC" ]] || DC_MAC="$(mac_of "$DC_IP")"
    [[ -n "$DC_MAC" ]] || die "no ARP entry for $DC_IP; pass -m"
    say "checking $CAPTURE for $DC_IP ($DC_MAC)"
fi

python3 -I - "$CAPTURE" "$DC_MAC" "$DC_IP" <<'EOF'
import re, sys

cap, mac, ip = open(sys.argv[1]).read(), sys.argv[2].lower(), sys.argv[3]

# One tcpdump -vvv record per packet: a timestamp line followed by indented lines.
pkts = []
for block in re.split(r'\n(?=\d\d:\d\d:\d\d\.\d+ )', cap):
    m = re.match(r'(\d\d:\d\d:\d\d\.\d+)', block)
    if not m or 'BOOTP/DHCP' not in block:
        continue
    low = block.lower()
    def grab(pattern, default=''):
        mm = re.search(pattern, block)
        return mm.group(1).strip() if mm else default
    p = {
        'time': m.group(1),
        'type': grab(r'DHCP-Message \(53\), length 1: (\w+)', '?'),
        'xid': grab(r'xid (0x[0-9a-f]+)', '?'),
        'src': grab(r'\n\s+([\d.]+)\.\d+ > ', '?'),
        'from_dc': f'request from {mac}' in low,
        'to_dc': 'reply' in low and f'client-ethernet-address {mac}' in low,
        'yiaddr': grab(r'Your-IP ([\d.]+)'),
        'hostname': grab(r'Hostname \(12\), length \d+: "([^"]*)"'),
    }
    if p['from_dc'] or p['to_dc']:
        pkts.append(p)

def hms(t):
    h, m, s = t.split(':'); return int(h) * 3600 + int(m) * 60 + float(s)

print()
print(f"DHCP packets involving the console ({mac}):")
for p in pkts:
    who = 'console ->' if p['from_dc'] else '<- server '
    extra = f"yiaddr {p['yiaddr']}" if p['to_dc'] else f"from {p['src']}, hostname \"{p['hostname']}\""
    print(f"  {p['time']}  {who} {p['type']:<8} xid {p['xid']}  {extra}")
if not pkts:
    print("  (none)")
print()

fails = []
def check(ok, what):
    print(('  ok   ' if ok else '  FAIL ') + what)
    if not ok: fails.append(what)
def skip(what):
    print('  skip ' + what)

sent = [p for p in pkts if p['from_dc']]
got = [p for p in pkts if p['to_dc']]
discovers = [p for p in sent if p['type'] == 'Discover']
offers = [p for p in got if p['type'] == 'Offer']
acks = [p for p in got if p['type'] == 'ACK']
naks = [p for p in got if p['type'] == 'NAK']

if sent:
    check(len(discovers) >= 1, "console sent a DISCOVER")
    check(all(p['src'] == '0.0.0.0' for p in sent if p['type'] in ('Discover', 'Request')),
          "DISCOVER and REQUEST sent from 0.0.0.0")
    check(all(p['hostname'] for p in sent), "hostname option in every packet from the console")
    xids = [p['xid'] for p in discovers]
    check(len(set(xids)) == len(xids), "each DISCOVER used a different transaction ID")
else:
    print("  note: none of the console's own packets were captured (Wi-Fi?), judging from the server's replies")
    skip("checks on the console's DISCOVER/REQUEST contents")

check(len(acks) >= 1, "server ACKed the console")
if acks:
    first_ack = hms(acks[0]['time'])
    check(acks[-1]['yiaddr'] == ip, f"last ACK gave {acks[-1]['yiaddr']}, dcload-ip answers on {ip}")
    # Every reply the server sent the console should be part of the same
    # exchange; a second xid or a new OFFER after the ACK means it started over.
    reply_xids = {p['xid'] for p in got}
    check(len(reply_xids) == 1, "all server replies to the console belong to one transaction"
          + (f" ({len(reply_xids)} different xids)" if len(reply_xids) != 1 else ''))
    check(not [p for p in offers if hms(p['time']) > first_ack], "no new OFFER after the console was ACKed")
    late = [p for p in sent if hms(p['time']) > first_ack]
    check(not late, "nothing more from the console after it was ACKed"
          + (f" ({len(late)} packet(s) after {acks[0]['time']})" if late else ''))
    start = discovers[0] if discovers else (offers[0] if offers else None)
    if start:
        what = 'first DISCOVER' if discovers else 'OFFER'
        print(f"       bound {first_ack - hms(start['time']):.2f}s after the {what}")
    if len(discovers) > 1 and all(hms(p['time']) < first_ack for p in discovers):
        print(f"       note: {len(discovers)} DISCOVERs before the ACK (retries while waiting for the server)")
check(not naks, "no NAK to the console")

print()
print("ALL PASSED" if not fails else f"FAILED: {len(fails)} check(s)")
sys.exit(1 if fails else 0)
EOF
