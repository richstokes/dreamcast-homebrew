#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["cryptography>=44,<47"]
# ///
"""Run https.c's SH-4 checks in Flycast against local HTTPS servers.

One server per certificate case, all on --host (this Mac's LAN address, which
Flycast's BBA proxy can reach), signed by throwaway CAs made for the run.
"""
import argparse
from datetime import datetime, timedelta, timezone
import http.server
import ipaddress
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

COMMON = Path(__file__).resolve().parents[1]


def name(text):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, text)])


def issue(subject, key, issuer, issuer_key, start, end, *, ca=False, ip=None):
    builder = (x509.CertificateBuilder().subject_name(name(subject))
               .issuer_name(issuer.subject if issuer else name(subject))
               .public_key(key.public_key()).serial_number(x509.random_serial_number())
               .not_valid_before(start).not_valid_after(end)
               .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if ca:
        builder = builder.add_extension(x509.KeyUsage(
            digital_signature=True, key_cert_sign=True, crl_sign=True, content_commitment=False,
            key_encipherment=False, data_encipherment=False, key_agreement=False,
            encipher_only=False, decipher_only=False), critical=True)
    if ip:
        builder = builder.add_extension(x509.SubjectAlternativeName(
            [x509.IPAddress(ipaddress.ip_address(ip))]), critical=False)
    return builder.sign(issuer_key or key, hashes.SHA256())


class Answer(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'   # keep the connection, so reuse can be checked

    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Length', '2')
        self.end_headers()
        self.wfile.write(b'ok')

    def log_message(self, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host')
    parser.add_argument('--log-dir', required=True, type=Path)
    parser.add_argument('--flycast', type=Path, default=Path.home() /
                        '.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast')
    parser.add_argument('--kos-env', type=Path, default=Path.home() /
                        '.local/share/dreamcast/kos/environ.sh')
    args = parser.parse_args()
    if not args.host:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(('1.1.1.1', 443))
            args.host = probe.getsockname()[0]
    ipaddress.IPv4Address(args.host)
    args.log_dir.mkdir(parents=True, exist_ok=True)

    now = datetime.now(timezone.utc)
    day = timedelta(days=1)
    keys = {}

    def key(label):
        keys[label] = ec.generate_private_key(ec.SECP256R1())
        return keys[label]

    ca = issue('https checks CA', key('ca'), None, None, now - 30 * day, now + 3650 * day, ca=True)
    old_ca = issue('https checks 2019 CA', key('old_ca'), None, None,
                   datetime(2019, 1, 1, tzinfo=timezone.utc), datetime(2041, 1, 1, tzinfo=timezone.utc), ca=True)
    stranger = issue('untrusted CA', key('stranger'), None, None, now - 30 * day, now + 3650 * day, ca=True)
    lapsed = issue('expired intermediate', key('lapsed'), ca, keys['ca'], now - 60 * day, now - 3 * day, ca=True)
    host = args.host
    # name: (certificate chain, key)
    cases = {
        'GOOD': ([issue('good', key('good'), ca, keys['ca'], now - day, now + 30 * day, ip=host)], 'good'),
        'EXPIRED': ([issue('expired', key('expired'), ca, keys['ca'], now - 30 * day, now - 3 * day, ip=host)], 'expired'),
        'FUTURE': ([issue('future', key('future'), ca, keys['ca'], now + 3 * day, now + 30 * day, ip=host)], 'future'),
        'FRESH': ([issue('fresh', key('fresh'), ca, keys['ca'], now - timedelta(minutes=10), now + 30 * day, ip=host)], 'fresh'),
        'RECENTLY_EXPIRED': ([issue('recent', key('recent'), ca, keys['ca'], now - 30 * day, now - timedelta(hours=2), ip=host)], 'recent'),
        'EXPIRED_INTERMEDIATE': ([issue('chained', key('chained'), lapsed, keys['lapsed'], now - day, now + 30 * day, ip=host), lapsed], 'chained'),
        'UNTRUSTED': ([issue('untrusted', key('untrusted'), stranger, keys['stranger'], now - day, now + 30 * day, ip=host)], 'untrusted'),
        'WRONG_HOST': ([issue('wrong host', key('wrong'), ca, keys['ca'], now - day, now + 30 * day, ip='10.9.8.7')], 'wrong'),
        'LONG': ([issue('long', key('long'), old_ca, keys['old_ca'], datetime(2020, 1, 1, tzinfo=timezone.utc),
                        datetime(2040, 1, 1, tzinfo=timezone.utc), ip=host)], 'long'),
    }

    servers = []
    with tempfile.TemporaryDirectory(prefix='https-checks-') as directory:
        work = Path(directory)
        certs = work / 'certs'
        certs.mkdir()
        ports = {}
        for case, (chain, label) in cases.items():
            pem = certs / f'{case}.pem'
            pem.write_bytes(b''.join(c.public_bytes(serialization.Encoding.PEM) for c in chain))
            key_file = certs / f'{case}.key'
            key_file.write_bytes(keys[label].private_bytes(serialization.Encoding.PEM,
                                 serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(pem, key_file)
            server = http.server.ThreadingHTTPServer((host, 0), Answer)
            server.daemon_threads = True
            server.socket = context.wrap_socket(server.socket, server_side=True)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            servers.append(server)
            ports[case] = server.server_port

        # Flycast moves a real card into a custom VMU folder that lacks it and
        # deletes the original, so give every slot a blank card of its own.
        (work / 'vmus').mkdir()
        for slot in ('A1', 'A2', 'B1', 'B2', 'C1', 'C2', 'D1', 'D2'):
            (work / 'vmus' / f'vmu_save_{slot}.bin').write_bytes(bytes(131072))
        build = work / 'tests'
        (build / 'romdisk').mkdir(parents=True)
        for file in ('https.c', 'https.h'):
            shutil.copyfile(COMMON / file, work / file)
        shutil.copyfile(COMMON / 'tests/https_checks.c', build / 'https_checks.c')
        (build / 'romdisk/test-ca.pem').write_bytes(
            ca.public_bytes(serialization.Encoding.PEM) + old_ca.public_bytes(serialization.Encoding.PEM))
        (build / 'config.h').write_text(f'#define HOST "{host}"\n#define BUILT_AT {int(time.time())}LL\n' + ''.join(
            f'#define PORT_{case} {port}\n' for case, port in ports.items()))
        (build / 'Makefile').write_text('''TARGET = checks.elf
OBJS = https_checks.o romdisk.o
KOS_BUILD_SUBARCHS = pristine
KOS_ROMDISK_DIR = romdisk
all: $(TARGET)
include $(KOS_BASE)/Makefile.rules
$(TARGET): $(OBJS)
\tkos-cc -o $@ $(OBJS) -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lm -lpthread
''')
        with (args.log_dir / 'build.log').open('w') as output:
            subprocess.run(['bash', '-c', 'source "$1" && make -C "$2" && '
                            'file "$2/checks.elf" && sh-elf-readelf -h "$2/checks.elf"',
                            'https-checks', str(args.kos_env.resolve()), str(build)],
                           check=True, stdout=output, stderr=output)
        config = ('network:EmulateBBA=yes,network:DCNet=no,audio:backend=null,'
                  'config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no,'
                  f'config:Dreamcast.VMUPath={work / "vmus"},config:PerGameVmu=no')
        serial = args.log_dir / 'flycast.log'
        marker = 'HTTPS CHECKS PASSED'
        try:
            with serial.open('w') as output:
                process = subprocess.Popen([str(args.flycast.resolve()), '-config', config,
                                            str(build / 'checks.elf')], cwd=work,
                                           stdout=output, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 240
                    while time.monotonic() < deadline:
                        log = serial.read_text(errors='replace')
                        if marker in log or 'assertion' in log.lower() or process.poll() is not None:
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
            for server in servers:
                server.shutdown()
                server.server_close()
        passed = marker in serial.read_text(errors='replace')
        summary = f'{"PASS" if passed else "FAIL"}: HTTPS chain, host name and date checks\n'
        (args.log_dir / 'result.txt').write_text(summary)
        print(summary, end='')
        return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
