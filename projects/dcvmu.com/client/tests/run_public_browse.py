#!/usr/bin/env python3
"""Run public browsing through Flycast against the actual local HTTPS service.

Use service/.venv/bin/python. Only temporary synthetic accounts and VMUs are
used. --host must be this Mac's LAN IPv4 address for Flycast's picoTCP proxy.
"""
import argparse
import hashlib
import io
import re
import http.server
import ipaddress
from pathlib import Path
import shutil
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs

CLIENT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CLIENT.parent / 'service'))
from app import create_app, PASSWORDS
from test_service import vms


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', action='store_true', help='Test icon studio and card imports through the client')
    parser.add_argument('--host', required=True, type=ipaddress.IPv4Address)
    parser.add_argument('--log-dir', required=True, type=Path)
    parser.add_argument('--modem', action='store_true',
                        help='Use Flycast modem/PPP emulation instead of BBA')
    parser.add_argument('--flycast', type=Path, default=Path.home() /
                        '.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast')
    parser.add_argument('--kos-env', type=Path, default=Path.home() /
                        '.local/share/dreamcast/kos/environ.sh')
    args = parser.parse_args()
    marker = 'VMU TOOLS SELF-TEST' if args.tools else 'PUBLIC BROWSE SELF-TEST'
    args.log_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dcvmu-public-browse-') as directory:
        work = Path(directory)
        for name in ('romdisk', 'tests', 'vmus'):
            (work / name).mkdir()
        for name in ('dcvmu-client.c', 'net.c', 'modem.c', 'auth.c', 'client.h', 'Makefile',
                     'romdisk/cacert.pem', 'tests/public_browse.c', 'tests/vmu_tools.c'):
            shutil.copyfile(CLIENT / name, work / name)
        database = work / 'fixture.sqlite3'
        app = create_app({'DATABASE': str(database)})
        raw = vms(icons=1, palette=[0xF08F] * 16, frames=bytes(512))
        with sqlite3.connect(database) as db:
            for user in ('reader', 'sharer'):
                db.execute('INSERT INTO users(username,email,password_hash,created) VALUES (?,?,?,0)',
                           (user, user + '@example.invalid', PASSWORDS.hash('local-fixture-password')))
            for i in range(0 if args.tools else 10):
                db.execute('INSERT INTO saves(user_id,name,filename,game,notes,private,data,sha256,created,updated) '
                           'VALUES (?,?,?,?,?,?,?,?,0,123)',
                           (1 if i == 0 else 2, 'Private' if i < 2 else 'Save ' + str(i),
                            'SAVE_' + str(i), 'Power Stone' if 2 <= i < 5 else 'Other Game',
                            '', int(i < 2), raw, hashlib.sha256(raw).hexdigest()))

        if args.tools:
            from test_vmu_tools import card_image
            from vmu_tools import ICON_NAME
            with app.test_client() as web:
                def csrf(path):
                    return re.search(r'name="csrf" value="([^"]+)"', web.get(path, base_url='https://dcvmu.com').text)[1]
                def post(path, data):
                    response = web.post(path, base_url='https://dcvmu.com', data=data)
                    assert response.status_code == 303, response.text
                    return response.location
                post('/login', dict(csrf=csrf('/login'), username='reader', password='local-fixture-password'))
                post('/studio', dict(csrf=csrf('/studio'), name='Studio icons', label='TOOLS TEST',
                    mono='10'*512, pixels='12'*512, palette='fffff06cff60'+'f000'*13, private='1', unlock='1'))
                card = card_image([('TEST_SAVE', vms(payload=b'preserved-data'*100, offset=1), 0x33, 1),
                                   ('DCVMU_AUTH', vms(payload=b'DCVMU-AUTH-V1 disposable'), 0x33, 0)])
                preview = post('/import', dict(csrf=csrf('/import'), image=(io.BytesIO(card), 'fixture.bin')))
                post(preview, dict(csrf=csrf(preview), entry='0', private='1'))
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT count(*) FROM saves').fetchone()[0] == 2

        original = app.wsgi_app
        failed_page = False
        def fixture(environ, start_response):
            nonlocal failed_page
            query = parse_qs(environ.get('QUERY_STRING', ''))
            if not args.tools and environ['PATH_INFO'] == '/api/v1/saves':
                if query.get('user') == ['ShArEr'] and query.get('page') == ['1'] and not failed_page:
                    failed_page = True
                    start_response('503 Service Unavailable', [('Content-Type', 'text/plain'), ('Content-Length', '19')])
                    return [b'Test page failure.\n']
                if query.get('user') == ['legacy']:
                    environ['QUERY_STRING'] = 'scope=public&user=sharer'
            return original(environ, start_response)

        app.wsgi_app = fixture

        class Handler(http.server.BaseHTTPRequestHandler):
            # Match the production service's keep-alive behavior and avoid
            # connection churn during rapid sequences of icon-range requests.
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_args):
                pass

            def handle_request(self):
                body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
                with app.test_client() as client:
                    response = client.open(self.path, base_url=f'https://{args.host}', method=self.command, data=body,
                                           headers=dict(self.headers))
                    payload = response.data
                    self.send_response(response.status_code)
                    for key, value in response.headers:
                        if key.lower() not in ('content-length', 'connection', 'transfer-encoding'):
                            self.send_header(key, value)
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)

            do_GET = handle_request
            do_POST = handle_request

        cert, key = work / 'romdisk/test-ca.pem', work / 'key.pem'
        with (args.log_dir / 'build.log').open('w') as output:
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                            '-keyout', str(key), '-out', str(cert), '-days', '2',
                            '-subj', f'/CN={args.host}', '-addext', f'subjectAltName=IP:{args.host}'],
                           check=True, stdout=output, stderr=output)
            server = http.server.ThreadingHTTPServer((str(args.host), 0), Handler)
            server.daemon_threads = True
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            (work / 'romdisk/test-service.txt').write_text(f'https://{args.host}:{server.server_port}\n')
            subprocess.run(['bash', '-c', 'source "$1" && make -C "$2" '
                            f'CPPFLAGS="-DDCVMU_DOWNLOAD_TEST -D{"DCVMU_TOOLS_TEST" if args.tools else "DCVMU_PUBLIC_TEST"}" && '
                            'file "$2/dcvmu-client.elf" && sh-elf-readelf -h "$2/dcvmu-client.elf"',
                            'public-build', str(args.kos_env.resolve()), str(work)],
                           check=True, stdout=output, stderr=output)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        serial = args.log_dir / 'flycast.log'
        # This silent client needs no audio device. SDL audio can block the
        # emulation thread when macOS changes output devices during automation.
        config = (f'network:EmulateBBA={"no" if args.modem else "yes"},network:DCNet=no,audio:backend=null,'
                  'config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no,'
                  'input:device1=0,input:device1.1=1,input:device1.2=1,'
                  f'config:Dreamcast.VMUPath={work / "vmus"},config:PerGameVmu=no')
        try:
            with serial.open('w') as output:
                process = subprocess.Popen([str(args.flycast.resolve()), '-config', config,
                                            str(work / 'dcvmu-client.elf')], cwd=work,
                                           stdout=output, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + (600 if args.modem else 240)
                    while time.monotonic() < deadline:
                        log = serial.read_text(errors='replace')
                        if (marker + ' PASSED' in log or
                                marker + ' FAILED' in log or process.poll() is not None):
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
        log = serial.read_text(errors='replace')
        passed = marker + ' PASSED' in log and (args.tools or failed_page)
        summary = ('PASS' if passed else 'FAIL') + f': {"VMU tools" if args.tools else "public browse"} integration in Flycast ({"modem/PPP" if args.modem else "BBA"})\n'
        (args.log_dir / 'result.txt').write_text(summary)
        print(summary, end='')
        return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
