#!/usr/bin/env python3
"""Exercise the actual KOS HTTPS upload code through Flycast's TCP proxy.

Builds only in a temporary directory and uses synthetic data, a local TLS
fixture, and isolated VMUs. No public service, account, or personal VMU needed.
Pass --host with this Mac's LAN IPv4 address, reachable by the picoTCP proxy.
"""
import argparse
from email import policy
from email.parser import BytesParser
import http.server
import ipaddress
from pathlib import Path
import queue
import shutil
import ssl
import subprocess
import tempfile
import threading
import time

CLIENT = Path(__file__).resolve().parents[1]
SIZES = (4608, 8704, 98816)
DATA = bytes((i * 31 + 7) & 255 for i in range(max(SIZES)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, type=ipaddress.IPv4Address)
    parser.add_argument('--flycast', type=Path, default=Path.home() /
                        '.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast')
    parser.add_argument('--kos-env', type=Path, default=Path.home() /
                        '.local/share/dreamcast/kos/environ.sh')
    parser.add_argument('--log-dir', type=Path, required=True)
    parser.add_argument('--modem', action='store_true', help='Use Flycast modem/PPP emulation')
    parser.add_argument('--transfers', type=int, default=30,
                        help='Upload count (multiple of three); each run also checks a large download')
    args = parser.parse_args()
    if args.transfers < 3 or args.transfers % 3:
        parser.error('--transfers must be a positive multiple of three')
    args.log_dir.mkdir(parents=True, exist_ok=True)
    results = queue.Queue()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *_args):
            pass

        def do_GET(self):
            if (self.path != '/api/v1/saves/1/download?revision=1' or
                    self.headers.get('Authorization') != 'Bearer synthetic-test-token'):
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Length', str(len(DATA)))
            self.end_headers()
            self.wfile.write(DATA)
            self.wfile.flush()

        def do_POST(self):
            try:
                self.connection.settimeout(70)
                length = int(self.headers['Content-Length'])
                assert 0 < length < 150000, 'invalid body length'
                body = self.rfile.read(length)
                assert len(body) == length, 'truncated request body'
                message = BytesParser(policy=policy.default).parsebytes(
                    ('Content-Type: ' + self.headers['Content-Type'] +
                     '\r\nMIME-Version: 1.0\r\n\r\n').encode() + body)
                fields = {part.get_param('name', header='content-disposition'):
                          part.get_payload(decode=True) for part in message.iter_parts()}
                payload = fields['save']
                assert self.path == '/api/v1/saves', 'unexpected path'
                assert self.headers['Authorization'] == 'Bearer synthetic-test-token'
                assert fields['private'] == b'1' and fields['mode'] == b'ask'
                assert len(payload) in SIZES and payload == DATA[:len(payload)], 'corrupt save'
                results.put(len(payload))
                self.send_response(201)
                self.send_header('Content-Length', '3')
                self.end_headers()
                self.wfile.write(b'OK\n')
                self.wfile.flush()
            except Exception as error:
                results.put(str(error))
                self.close_connection = True

    with tempfile.TemporaryDirectory(prefix='dcvmu-upload-stress-') as directory:
        work = Path(directory)
        (work / 'romdisk').mkdir()
        (work / 'vmus').mkdir()
        for name in ('net.c', 'modem.c', 'client.h'):
            shutil.copyfile(CLIENT / name, work / name)
        shutil.copyfile(CLIENT / 'tests/upload_stress.c', work / 'stress.c')
        shutil.copyfile(CLIENT / 'romdisk/cacert.pem', work / 'romdisk/cacert.pem')
        cert = work / 'romdisk/test-ca.pem'
        key = work / 'key.pem'
        with (args.log_dir / 'build.log').open('w') as build_log:
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                            '-keyout', str(key), '-out', str(cert), '-days', '2',
                            '-subj', f'/CN={args.host}', '-addext',
                            f'subjectAltName=IP:{args.host}'],
                           check=True, stdout=build_log, stderr=build_log)
            server = http.server.ThreadingHTTPServer((str(args.host), 0), Handler)
            server.daemon_threads = True
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            origin = f'https://{args.host}:{server.server_port}'
            (work / 'romdisk/test-service.txt').write_text(origin + '\n')
            (work / 'Makefile').write_text('''TARGET = stress.elf
OBJS = stress.o net.o modem.o romdisk.o
KOS_BUILD_SUBARCHS = pristine
KOS_ROMDISK_DIR = romdisk
KOS_CSTD = -std=gnu17
STRESS_COUNT = 30
CPPFLAGS = -DDCVMU_DOWNLOAD_TEST -DDCVMU_STRESS_COUNT=$(STRESS_COUNT)
all: $(TARGET)
include $(KOS_BASE)/Makefile.rules
$(TARGET): $(OBJS)
\tkos-cc -o $@ $(OBJS) -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lm -lpthread -lppp
''')
            subprocess.run(['bash', '-c', 'source "$1" && make -C "$2" "STRESS_COUNT=$3" && '
                            'file "$2/stress.elf" && sh-elf-readelf -h "$2/stress.elf"',
                            'stress-build', str(args.kos_env.resolve()), str(work), str(args.transfers)],
                           check=True, stdout=build_log, stderr=build_log)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        serial = args.log_dir / 'flycast.log'
        config = (f'network:EmulateBBA={"no" if args.modem else "yes"},network:DCNet=no,audio:backend=null,'
                  'config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no,'
                  f'config:Dreamcast.VMUPath={work / "vmus"},config:PerGameVmu=no')
        try:
            with serial.open('w') as output:
                process = subprocess.Popen([str(args.flycast.resolve()), '-config', config,
                                            str(work / 'stress.elf')], cwd=work,
                                           stdout=output, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + (300 + args.transfers * 60 if args.modem else 240)
                    while time.monotonic() < deadline:
                        log = serial.read_text(errors='replace')
                        if ('UPLOAD STRESS PASSED:' in log or 'UPLOAD STRESS FAILED:' in log
                                or process.poll() is not None):
                            break
                        time.sleep(0.25)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        verified = []
        while not results.empty():
            verified.append(results.get_nowait())
        log = serial.read_text(errors='replace')
        passed = (f'UPLOAD STRESS PASSED: {args.transfers}/{args.transfers}' in log and
                  'LARGE DOWNLOAD PASSED' in log and verified == list(SIZES) * (args.transfers // 3))
        summary = f'{"PASS" if passed else "FAIL"}: {len(verified)}/{args.transfers} uploads and large download, payload checks: {verified}\n'
        (args.log_dir / 'result.txt').write_text(summary)
        print(summary, end='')
        return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
