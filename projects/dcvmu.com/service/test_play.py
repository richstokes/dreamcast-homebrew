"""Web player page, release asset parsing and the ROM cache Caddy serves from."""
import io
import os
import stat
import tempfile
import time
import unittest
import urllib.error
from email.message import Message
from pathlib import Path
from unittest.mock import patch

import play
from app import create_app


class Upstream(io.BytesIO):
    def __init__(self, data, etag=None):
        super().__init__(data)
        self.headers = Message()
        if etag:
            self.headers['ETag'] = etag


def http_error(code):
    return urllib.error.HTTPError('https://github.com/', code, 'error', Message(), None)


class ParseTest(unittest.TestCase):
    def test_accepted_forms(self):
        release = 'https://github.com/richstokes/dreamcast-homebrew/releases'
        for value, expected in (
                ('gravity-wave.elf', ('continuous', 'gravity-wave.elf')),
                (' /continuous/dreamkarts.cdi/ ', ('continuous', 'dreamkarts.cdi')),
                ('v1.0/chroma-circuit.ELF', ('v1.0', 'chroma-circuit.ELF')),
                (release + '/download/continuous/ping-cube.elf', ('continuous', 'ping-cube.elf')),
                (release + '/latest/download/ping-cube.cdi', ('latest', 'ping-cube.cdi'))):
            self.assertEqual(play.parse_release_asset(value), expected)

    def test_rejected_forms(self):
        for value in ('', 'readme.txt', '../ping-cube.elf', '../../ping-cube.elf', 'a/b/c.elf',
                      'continuous/.hidden.elf', 'http://github.com/richstokes/dreamcast-homebrew/releases/download/continuous/x.elf',
                      'https://example.com/richstokes/dreamcast-homebrew/releases/download/continuous/x.elf',
                      'https://github.com/someone/else/releases/download/continuous/x.elf',
                      'https://github.com/richstokes/dreamcast-homebrew/archive/main.zip',
                      'continuous/game%20name.elf'):
            with self.assertRaises(ValueError, msg=value):
                play.parse_release_asset(value)

    def test_title(self):
        self.assertEqual(play.title('drift-los-angeles.elf'), 'Drift Los Angeles')
        self.assertEqual(play.title('pico8_dreamcast.cdi'), 'Pico8 Dreamcast')


class PlayTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        # Production runs with UMask=0077; Caddy still has to read the cache.
        umask = os.umask(0o077)
        self.addCleanup(os.umask, umask)
        self.cache = Path(self.temp.name) / 'cache'
        self.app = create_app({'TESTING': True, 'DATABASE': self.temp.name + '/play.sqlite3', 'CACHE': str(self.cache)})
        self.client = self.app.test_client()
        self.rom = self.cache / 'roms' / 'continuous' / 'dreamkarts.cdi'

    def fetch(self, *results):
        return patch('play.urllib.request.urlopen', side_effect=list(results))

    def assert_public(self, path):
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o644)
        for directory in path.parents:
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o755)
            if directory == self.cache:
                break

    def test_setup_page(self):
        r = self.client.get('/play')
        self.assertEqual(r.status_code, 200)
        self.assertIn('name="rom"', r.text)
        self.assertNotIn('dreamcast-game', r.text)
        self.assertEqual(r.headers['X-Robots-Tag'], 'noindex, nofollow')
        self.assertEqual(r.headers['Cross-Origin-Embedder-Policy'], 'require-corp')
        self.assertEqual(r.headers['Cross-Origin-Opener-Policy'], 'same-origin')
        self.assertIn('https://cdn.emulatorjs.org', r.headers['Content-Security-Policy'])
        self.assertEqual(r.headers['Cache-Control'], 'no-store')

    def test_player_page(self):
        r = self.client.get('/play?rom=continuous%2Fdreamkarts.cdi&name=DreamKarts')
        self.assertEqual(r.status_code, 200)
        self.assertIn('data-rom="/play/rom/continuous/dreamkarts.cdi"', r.text)
        self.assertIn('data-name="DreamKarts"', r.text)
        self.assertRegex(r.text, r'data-core="/play/flycast-wasm.data\?v=[0-9a-f]{16}"')
        self.assertIn('href="https://github.com/richstokes/dreamcast-homebrew/releases/download/continuous/dreamkarts.cdi"', r.text)
        self.assertIn('<script src="/static/play.js"></script>', r.text)
        r = self.client.get('/play?rom=drift-los-angeles.elf&name=%3Cb%3E%0A')
        self.assertIn('data-name="&lt;b&gt;"', r.text)
        r = self.client.get('/play?rom=drift-los-angeles.elf')
        self.assertIn('<h1>Drift Los Angeles</h1>', r.text)

    def test_invalid_rom_shows_setup(self):
        r = self.client.get('/play?rom=https%3A%2F%2Fexample.com%2Fgame.elf')
        self.assertEqual(r.status_code, 400)
        self.assertIn('Use a release URL from github.com/richstokes/dreamcast-homebrew.', r.text)
        self.assertIn('value="https://example.com/game.elf"', r.text)

    def test_other_pages_keep_strict_headers(self):
        r = self.client.get('/getting-started')
        self.assertIn("script-src 'self';", r.headers['Content-Security-Policy'])
        self.assertNotIn('Cross-Origin-Embedder-Policy', r.headers)

    def test_rom_miss_fetches_into_cache(self):
        with self.fetch(Upstream(b'disc', '"v1"')) as urlopen:
            r = self.client.get('/play/rom/continuous/dreamkarts.cdi')
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.data, b'')
        self.assertEqual(r.headers['X-Dcvmu-File'], '/roms/continuous/dreamkarts.cdi')
        self.assertEqual(r.headers['Cache-Control'], 'public, max-age=300')
        self.assertEqual(r.headers['Content-Type'], 'application/octet-stream')
        request = urlopen.call_args[0][0]
        self.assertEqual(request.full_url, 'https://github.com/richstokes/dreamcast-homebrew/releases/download/continuous/dreamkarts.cdi')
        self.assertIsNone(request.get_header('If-none-match'))
        self.assertEqual(self.rom.read_bytes(), b'disc')
        self.assertEqual(self.rom.with_name('dreamkarts.cdi.etag').read_text(), '"v1"')
        self.assert_public(self.rom)
        self.assertEqual(sorted(os.listdir(self.rom.parent)), ['dreamkarts.cdi', 'dreamkarts.cdi.etag'])

    def test_latest_tag_url(self):
        with self.fetch(Upstream(b'elf')) as urlopen:
            r = self.client.get('/play/rom/latest/ping-cube.elf')
        self.assertEqual(r.headers['X-Dcvmu-File'], '/roms/latest/ping-cube.elf')
        self.assertEqual(urlopen.call_args[0][0].full_url,
                         'https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ping-cube.elf')

    def test_fresh_rom_skips_github(self):
        with self.fetch(Upstream(b'disc', '"v1"')):
            self.client.get('/play/rom/continuous/dreamkarts.cdi')
        with self.fetch() as urlopen:
            r = self.client.head('/play/rom/continuous/dreamkarts.cdi')
        self.assertEqual(r.status_code, 200)
        urlopen.assert_not_called()

    def test_stale_rom_revalidates(self):
        with self.fetch(Upstream(b'disc', '"v1"')):
            self.client.get('/play/rom/continuous/dreamkarts.cdi')
        old = time.time() - play.FRESH_SECONDS - 1
        os.utime(self.rom, (old, old))
        with self.fetch(http_error(304)) as urlopen:
            r = self.client.get('/play/rom/continuous/dreamkarts.cdi')
        self.assertEqual(r.status_code, 200)
        self.assertEqual(urlopen.call_args[0][0].get_header('If-none-match'), '"v1"')
        self.assertFalse(play.stale(self.rom))
        os.utime(self.rom, (old, old))
        with self.fetch(Upstream(b'new disc', '"v2"')):
            self.client.get('/play/rom/continuous/dreamkarts.cdi')
        self.assertEqual(self.rom.read_bytes(), b'new disc')
        self.assertEqual(self.rom.with_name('dreamkarts.cdi.etag').read_text(), '"v2"')

    def test_outage_serves_stale_copy(self):
        with self.fetch(Upstream(b'disc')):
            self.client.get('/play/rom/continuous/dreamkarts.cdi')
        old = time.time() - play.FRESH_SECONDS - 1
        os.utime(self.rom, (old, old))
        for failure in (urllib.error.URLError('down'), http_error(503), TimeoutError()):
            with self.fetch(failure):
                r = self.client.get('/play/rom/continuous/dreamkarts.cdi')
            self.assertEqual(r.status_code, 200)
            self.assertEqual(self.rom.read_bytes(), b'disc')
            # The stale copy waits another five minutes before the next attempt.
            self.assertFalse(play.stale(self.rom))
            os.utime(self.rom, (old, old))

    def test_fetch_failures_without_cache(self):
        with self.fetch(urllib.error.URLError('down')):
            r = self.client.get('/play/rom/continuous/dreamkarts.cdi')
        self.assertEqual((r.status_code, r.text), (502, 'GitHub release could not be loaded\n'))
        self.assertNotIn('X-Dcvmu-File', r.headers)
        with self.fetch(http_error(404)):
            r = self.client.get('/play/rom/continuous/missing.elf')
        self.assertEqual(r.status_code, 404)
        with patch('play.MAX_ASSET', 4), self.fetch(Upstream(b'too large')):
            r = self.client.get('/play/rom/continuous/dreamkarts.cdi')
        self.assertEqual(r.status_code, 502)
        self.assertEqual(os.listdir(self.rom.parent), [])

    def test_removed_asset_leaves_cache(self):
        with self.fetch(Upstream(b'disc', '"v1"')):
            self.client.get('/play/rom/continuous/dreamkarts.cdi')
        old = time.time() - play.FRESH_SECONDS - 1
        os.utime(self.rom, (old, old))
        with self.fetch(http_error(404)):
            r = self.client.get('/play/rom/continuous/dreamkarts.cdi')
        self.assertEqual(r.status_code, 404)
        self.assertEqual(os.listdir(self.rom.parent), [])

    def test_rejected_rom_paths(self):
        with self.fetch() as urlopen:
            for path in ('/play/rom/continuous/readme.txt', '/play/rom/.hidden/x.elf',
                         '/play/rom/continuous/.x.elf', '/play/rom/%2E%2E/x.elf', '/play/rom/-tag/x.elf'):
                r = self.client.get(path)
                self.assertEqual((r.status_code, r.text), (400, 'Release asset not allowed\n'), path)
            self.assertEqual(self.client.get('/play/rom/a/b/c.elf').status_code, 404)
        urlopen.assert_not_called()
        self.assertFalse(self.cache.exists())

    def test_core_is_copied_into_cache(self):
        version = self.client.get('/play?rom=ping-cube.elf').text.split('flycast-wasm.data?v=')[1][:16]
        r = self.client.get('/play/flycast-wasm.data?v=' + version)
        self.assertEqual(r.headers['X-Dcvmu-File'], '/player/flycast-wasm.data')
        self.assertEqual(r.headers['Cache-Control'], 'public, max-age=31536000, immutable')
        core = self.cache / 'player' / 'flycast-wasm.data'
        source = Path(self.app.static_folder) / 'play' / 'flycast-wasm.data'
        self.assertEqual(core.read_bytes(), source.read_bytes())
        self.assertEqual(core.stat().st_mtime_ns, source.stat().st_mtime_ns)
        self.assert_public(core)
        self.assertEqual(self.client.get('/play/flycast-wasm.data').headers['Cache-Control'], 'no-cache')
        # A changed bundle replaces the cached copy.
        core.write_bytes(b'stale')
        self.client.get('/play/flycast-wasm.data')
        self.assertEqual(core.read_bytes(), source.read_bytes())


if __name__ == '__main__':
    unittest.main()
