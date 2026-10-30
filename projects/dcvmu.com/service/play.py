"""Web player: release asset names and the on-disk cache Caddy sends files from.

GitHub release downloads lack the CORS headers an in-browser emulator needs, so
DCVMU fetches each asset once into the cache and Caddy serves it same-origin.
Gunicorn never streams the bytes itself, so slow downloads cannot tie up its
few threads.
"""
import os
import re
import tempfile
import time
import urllib.error
import urllib.request
from urllib.parse import quote, urlsplit

REPOSITORY = 'richstokes/dreamcast-homebrew'
RELEASES = f'https://github.com/{REPOSITORY}/releases'
EXTENSIONS = {'7z', 'bin', 'cdi', 'chd', 'cue', 'dat', 'elf', 'gdi', 'iso', 'lst', 'zip'}
NAME = re.compile(r'[A-Za-z0-9][A-Za-z0-9._-]*')
# Continuous assets are replaced in place, so recheck GitHub after five minutes.
FRESH_SECONDS = 300
# DreamKarts' disc carries ~210 MB of CD audio.
MAX_ASSET = 256 * 1024 * 1024
FETCH_SECONDS = 60


def check_asset(tag, asset):
    if not NAME.fullmatch(tag) or not NAME.fullmatch(asset):
        raise ValueError('The release tag or asset name contains unsupported characters.')
    if asset.rsplit('.', 1)[-1].lower() not in EXTENSIONS:
        raise ValueError('That release asset is not a supported Dreamcast image or executable.')


def parse_release_asset(value):
    """Return (tag, asset) for an asset name, a tag/asset path or a GitHub release URL."""
    raw = value.strip()
    if not raw:
        raise ValueError('Add a GitHub release asset URL or release path.')
    if re.match(r'https?://', raw, re.I):
        url = urlsplit(raw)
        if url.scheme != 'https' or url.hostname != 'github.com':
            raise ValueError(f'Use a release URL from github.com/{REPOSITORY}.')
        parts = [part for part in url.path.split('/') if part]
        if '/'.join(parts[:3]) == REPOSITORY + '/releases' and len(parts) == 6 and parts[3] == 'download':
            tag, asset = parts[4], parts[5]
        elif '/'.join(parts[:5]) == REPOSITORY + '/releases/latest/download' and len(parts) == 6:
            tag, asset = 'latest', parts[5]
        else:
            raise ValueError(f'Use a direct release download from github.com/{REPOSITORY}.')
    else:
        parts = raw.strip('/').split('/')
        if len(parts) == 1:
            tag, asset = 'continuous', parts[0]
        elif len(parts) == 2:
            tag, asset = parts
        else:
            raise ValueError('Use an asset name or a tag/asset path, such as continuous/gravity-wave.elf.')
    check_asset(tag, asset)
    return tag, asset


def title(asset):
    words = re.split(r'[-_]+', asset.rsplit('.', 1)[0])
    return ' '.join(word[:1].upper() + word[1:] for word in words if word)


def download_url(tag, asset):
    if tag == 'latest':
        return f'{RELEASES}/latest/download/{quote(asset)}'
    return f'{RELEASES}/download/{quote(tag)}/{quote(asset)}'


def public_dir(path):
    # The service runs with umask 0077; Caddy reads the cache as another user.
    if not path.is_dir():
        public_dir(path.parent)
        path.mkdir(exist_ok=True)
        path.chmod(0o755)


def stale(path):
    try:
        return time.time() - path.stat().st_mtime >= FRESH_SECONDS
    except FileNotFoundError:
        return True


def write_public(path, chunks):
    public_dir(path.parent)
    fd, temp = tempfile.mkstemp(dir=path.parent, prefix='.' + path.name)
    try:
        with os.fdopen(fd, 'wb') as out:
            for chunk in chunks:
                out.write(chunk)
        os.chmod(temp, 0o644)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def refresh(path, url):
    """Fetch url into path unless GitHub reports the cached ETag unchanged.

    Raises LookupError when the asset no longer exists, and OSError or
    ValueError when it cannot be fetched; a stale copy then waits another
    FRESH_SECONDS so an outage does not stall every request.
    """
    etag = path.with_name(path.name + '.etag')
    request = urllib.request.Request(url, headers={'Accept': 'application/octet-stream', 'User-Agent': 'dcvmu.com'})
    if path.exists() and etag.exists():
        # urllib repeats this header on GitHub's redirect to the storage host.
        request.add_header('If-None-Match', etag.read_text())
    deadline = time.monotonic() + FETCH_SECONDS

    def chunks(response):
        total = 0
        while chunk := response.read(65536):
            total += len(chunk)
            if total > MAX_ASSET or time.monotonic() > deadline:
                raise ValueError('Release asset is too large or too slow to fetch.')
            yield chunk

    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            write_public(path, chunks(response))
            if response.headers.get('ETag'):
                write_public(etag, [response.headers['ETag'].encode()])
            elif etag.exists():
                etag.unlink()
    except urllib.error.HTTPError as e:
        if e.code == 304:
            os.utime(path)
            return
        if e.code == 404:
            for stale_file in (path, etag):
                stale_file.unlink(missing_ok=True)
            raise LookupError(url) from e
        if path.exists():
            os.utime(path)
        raise
    except (OSError, ValueError):
        if path.exists():
            os.utime(path)
        raise


def bundle(source, path):
    """Copy a player file from static/ into the cache when it is missing or changed."""
    stat = source.stat()
    try:
        current = path.stat()
        if (current.st_size, current.st_mtime_ns) == (stat.st_size, stat.st_mtime_ns):
            return
    except FileNotFoundError:
        pass
    with source.open('rb') as data:
        write_public(path, iter(lambda: data.read(65536), b''))
    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
