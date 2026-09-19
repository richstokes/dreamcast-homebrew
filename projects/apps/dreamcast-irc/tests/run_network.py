#!/usr/bin/env python3
"""Exercise BBA/modem IRC against a local fixture, with temporary builds and VMUs."""
import argparse
import ipaddress
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import tempfile
import threading
import time

CLIENT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, type=ipaddress.IPv4Address)
    parser.add_argument('--log-dir', required=True, type=Path)
    parser.add_argument('--modem', action='store_true')
    parser.add_argument('--cancel-dial', action='store_true')
    parser.add_argument('--flycast', type=Path, default=Path.home() /
                        '.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast')
    parser.add_argument('--kos-env', type=Path, default=Path.home() /
                        '.local/share/dreamcast/kos/environ.sh')
    args = parser.parse_args()
    if args.cancel_dial and not args.modem:
        parser.error('--cancel-dial requires --modem')
    args.log_dir.mkdir(parents=True, exist_ok=True)
    events = queue.Queue()
    stop = threading.Event()
    listener = socket.socket()
    listener.bind((str(args.host), 0))
    listener.listen(3)
    listener.settimeout(0.5)
    port = listener.getsockname()[1]

    def serve():
        try:
            for round_number in range(1, 5 if args.modem else 4):
                while not stop.is_set():
                    try:
                        conn, _ = listener.accept()
                        break
                    except socket.timeout:
                        continue
                else:
                    return
                with conn:
                    conn.settimeout(30)
                    nick = None
                    pong = message = replied = False
                    stream = conn.makefile('rb')
                    def send(line):
                        conn.sendall((line + '\r\n').encode())
                    while not stop.is_set():
                        raw = stream.readline(1024)
                        if not raw:
                            break
                        line = raw.decode().rstrip('\r\n')
                        if line.startswith('NICK '):
                            nick = line.split()[1]
                        elif line.startswith('USER '):
                            assert nick
                            send(f':fixture 001 {nick} :Welcome')
                        elif line.startswith('JOIN '):
                            assert line == 'JOIN #fixture', line
                            send(f':{nick}!local@fixture JOIN :#fixture')
                            send(f':fixture!local@fixture PRIVMSG #fixture :fixture-ready-{round_number}')
                            send(f'PING :fixture-ping-{round_number}')
                        elif line == f'PONG :fixture-ping-{round_number}':
                            pong = True
                        elif line == f'PRIVMSG #fixture :fixture-out-{round_number}':
                            message = True
                        elif line.startswith('QUIT '):
                            events.put(f'quit-{round_number}')
                            break
                        if pong and message and not replied:
                            events.put(f'round-{round_number}')
                            send(f':fixture!local@fixture PRIVMSG #fixture :fixture-ok-{round_number}')
                            replied = True
                            if round_number == 1:
                                # Exercise the normal IRC retry path while PPP stays up.
                                time.sleep(0.3)
                                break
                    stream.close()
        except Exception as error:
            if not stop.is_set():
                events.put(f'ERROR: {error!r}')

    thread = threading.Thread(target=serve, daemon=True)
    try:
        with tempfile.TemporaryDirectory(prefix='dcirc-network-') as directory:
            work = Path(directory)
            (work / 'tests').mkdir()
            (work / 'vmus').mkdir()
            for name in ('dreamcast-irc.c', 'modem.c', 'modem.h', 'Makefile', 'tests/network.c'):
                shutil.copyfile(CLIENT / name, work / name)
            config_header = work / 'test-config.h'
            config_header.write_text(f'''#define IRC_SERVER "{args.host}"
#define IRC_PRIMARY_PORT "{port}"
#define IRC_FALLBACK_PORT "{port}"
#define IRC_CHANNEL "#fixture"
#define IRC_SELF_TEST 1
''' + ('#define IRC_TEST_CANCEL_DIAL 1\n' if args.cancel_dial else ''))
            with (args.log_dir / 'build.log').open('w') as output:
                subprocess.run(['bash', '-c', 'source "$1" && make -C "$2" "CPPFLAGS=-include $2/test-config.h" && '
                                'file "$2/dreamcast-irc.elf" && sh-elf-readelf -h "$2/dreamcast-irc.elf"',
                                'irc-test', str(args.kos_env.resolve()), str(work)],
                               check=True, stdout=output, stderr=output)
            thread.start()
            config = (f'network:EmulateBBA={"no" if args.modem else "yes"},network:DCNet=no,'
                      'audio:backend=null,config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no,'
                      'input:device1=0,input:device1.1=1,input:device1.2=1,input:device2=5,'
                      f'config:Dreamcast.VMUPath={work / "vmus"},config:PerGameVmu=no')
            serial = args.log_dir / 'flycast.log'
            with serial.open('w') as output:
                process = subprocess.Popen([str(args.flycast.resolve()), '-config', config,
                                            str(work / 'dreamcast-irc.elf')], cwd=work,
                                           stdout=output, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 240
                    while time.monotonic() < deadline:
                        log = serial.read_text(errors='replace')
                        if any(marker in log for marker in ('clean shutdown', 'kernel panic',
                               'Fatal error', 'IRC NETWORK TEST FAILED')) or process.poll() is not None:
                            break
                        time.sleep(0.25)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            log = serial.read_text(errors='replace')
    finally:
        stop.set()
        if thread.is_alive():
            thread.join(timeout=2)
        listener.close()
    recorded = list(events.queue)
    if args.cancel_dial:
        passed = 'Modem connection canceled' in log and not recorded
    else:
        # QUIT is best-effort: KOS can still have its bytes queued when the
        # client closes TCP and hangs up. Require delivered chat/PONG replies
        # for every connection and completed app/PPP cleanup instead.
        expected = [f'round-{n}' for n in range(1, 5 if args.modem else 4)]
        passed = ('IRC NETWORK TEST PASSED' in log and
                  [event for event in recorded if not event.startswith('quit-')] == expected)
        if args.modem:
            passed = passed and log.count('PPP ready, IP') == 3 and 'Modem carrier lost' in log
        else:
            passed = passed and 'Dreamcast IRC netif: bba' in log and 'modem carrier' not in log
    passed = passed and 'clean shutdown' in log and not any(
        marker in log for marker in ('kernel panic', 'Fatal error', 'IRC NETWORK TEST FAILED'))
    summary = f'{"PASS" if passed else "FAIL"}: {"modem" if args.modem else "BBA"} IRC, cancel={args.cancel_dial}, events={recorded}\n'
    (args.log_dir / 'result.txt').write_text(summary)
    print(summary, end='')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
