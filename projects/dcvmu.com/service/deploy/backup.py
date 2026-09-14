#!/usr/bin/env python3
"""Back up DCVMU accounts and save blobs using SSH and SQLite's online backup API."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile

# Static remote program: no interpolated shell arguments or credentials.
REMOTE_BACKUP = r'''
import os, sqlite3, sys, tempfile
with tempfile.TemporaryDirectory(prefix="dcvmu-backup-") as directory:
    path = os.path.join(directory, "archive.sqlite3")
    source = sqlite3.connect("file:/var/lib/dcvmu/archive.sqlite3?mode=ro", uri=True, timeout=30)
    target = sqlite3.connect(path)
    try:
        source.backup(target, pages=256, sleep=0.1)
        # Export a standalone database, without WAL sidecars.
        target.execute("PRAGMA journal_mode=DELETE")
    finally:
        target.close()
        source.close()
    with open(path, "rb") as snapshot:
        while True:
            block = snapshot.read(1024 * 1024)
            if not block:
                break
            sys.stdout.buffer.write(block)
    sys.stdout.buffer.flush()
'''


def verify(snapshot):
    with sqlite3.connect(snapshot.as_uri() + '?mode=ro&immutable=1', uri=True) as db:
        if db.execute('PRAGMA integrity_check').fetchall() != [('ok',)]:
            raise RuntimeError('SQLite integrity check failed')
        if db.execute('PRAGMA foreign_key_check').fetchone() is not None:
            raise RuntimeError('Database contains broken references')
        accounts = db.execute('SELECT count(*) FROM users').fetchone()[0]
        saves = 0
        total_bytes = 0
        for data, expected in db.execute('SELECT data, sha256 FROM saves'):
            if hashlib.sha256(data).hexdigest() != expected:
                raise RuntimeError('An uploaded save failed its SHA-256 check')
            saves += 1
            total_bytes += len(data)
    digest = hashlib.sha256()
    with snapshot.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'accounts': accounts, 'saves': saves, 'save_bytes': total_bytes,
            'database_sha256': digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path,
                        default=Path.home() / 'Dropbox' / 'DCVMU_Backups',
                        help='Backup directory (default: ~/Dropbox/DCVMU_Backups)')
    parser.add_argument('--host', default='root@dcvmu.com', help='SSH destination')
    args = parser.parse_args()
    if not args.host or args.host.startswith('-') or any(c.isspace() for c in args.host):
        parser.error('Invalid SSH destination')
    os.umask(0o077)
    destination = args.destination.expanduser().resolve()
    destination.mkdir(parents=True, exist_ok=True, mode=0o700)
    timestamp = dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H-%M-%S.%fZ')
    final = destination / ('dcvmu-' + timestamp)
    stage = Path(tempfile.mkdtemp(prefix='.partial-', dir=destination))
    try:
        snapshot = stage / 'archive.sqlite3'
        print('Taking an online snapshot over SSH (the website stays running)...', flush=True)
        with snapshot.open('wb') as output:
            subprocess.run(['ssh', '-o', 'ConnectTimeout=20', '-o', 'ServerAliveInterval=15',
                            '-o', 'ServerAliveCountMax=3', args.host, 'python3 -'],
                           input=REMOTE_BACKUP.encode(), stdout=output, check=True, timeout=1800)
        report = verify(snapshot)
        report.update({'created_utc': timestamp, 'source': args.host,
                       'database': 'archive.sqlite3', 'format_version': 1})
        (stage / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        (stage / 'README.txt').write_text(
            'DCVMU database backup: accounts, password hashes, sessions, metadata, '
            'and all public/private save files. Treat this directory as private.\n\n'
            'Verified using SQLite integrity/foreign-key checks and every save SHA-256.\n'
            'manifest.json includes the database SHA-256 and record counts.\n\n'
            'Restore with the matching DCVMU service code. Stop only dcvmu.service, '
            'preserve the current database for rollback, replace '
            '/var/lib/dcvmu/archive.sqlite3 with this file (owner dcvmu:dcvmu, mode 0600), '
            'remove stale archive.sqlite3-wal and archive.sqlite3-shm files while the '
            'service is stopped, then start dcvmu.service and check /healthz. '
            'Do not stop Caddy or the game server.\n'
            'Application source, client ELF, and server configuration are not included.\n')
        stage.rename(final)
        print(f'Verified backup: {final}')
        print(f"{report['accounts']} accounts; {report['saves']} saves; {report['save_bytes']} save bytes")
    finally:
        if stage.exists():
            shutil.rmtree(stage)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, sqlite3.Error, subprocess.SubprocessError) as error:
        raise SystemExit(f'Backup failed: {error}')
