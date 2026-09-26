"""Whole-VMU archives: login scrubbing, web pages and the console API."""
import hashlib
import io
import re
import sqlite3
import struct
import tempfile
import unittest
from unittest.mock import patch

import test_service as fixtures
from test_vmu_tools import card_image, icon_data
from vmu_tools import AUTH_MARKER, FAT_FREE, extract_image, scrub_card, swap_words
from app import create_app, MAX_ARCHIVES

SAVE = fixtures.vms(payload=b'progress' * 300, icons=1, offset=1, palette=[0xF08F] * 16, frames=bytes(512))
GAME = fixtures.vms(payload=b'minigame' * 64)
AUTH = fixtures.vms(payload=b'DCVMU-AUTH-V1 secret-token-bytes')


def fixture_card(extra=()):
    return card_image([('TEST_SAVE', SAVE, 0x33, 1), ('ICONDATA_VMS', icon_data(), 0x33, 0),
                       ('DCVMU_AUTH', AUTH, 0x33, 0), ('MINIGAME', GAME, 0xCC, 1), *extra])


def directory_names(image):
    return {row['filename'] for row in extract_image(image, 'x.bin')}


class ScrubTest(unittest.TestCase):
    def test_login_save_removed_and_other_files_byte_exact(self):
        card = fixture_card()
        image, summary = scrub_card(card)
        self.assertEqual(len(image), len(card))
        self.assertNotIn(AUTH_MARKER, image)
        self.assertNotIn(b'DCVMU_AUTH', image)
        self.assertEqual(summary['removed'], 1)
        self.assertEqual([f['filename'] for f in summary['files']], ['TEST_SAVE', 'ICONDATA_VMS', 'MINIGAME'])
        self.assertEqual([f['kind'] for f in summary['files']], ['data', 'data', 'game'])
        self.assertEqual(directory_names(image), {'TEST_SAVE', 'ICONDATA_VMS', 'MINIGAME'})
        before = {row['filename']: row for row in extract_image(card, 'x.bin')}
        after = {row['filename']: row for row in extract_image(image, 'x.bin')}
        for name in ('TEST_SAVE', 'ICONDATA_VMS'):
            self.assertEqual(after[name]['data'], before[name]['data'])
            self.assertEqual(after[name]['header_offset'], before[name]['header_offset'])
        # System blocks and directory entries of kept files are untouched.
        self.assertEqual(image[255*512:], card[255*512:])
        fat = struct.unpack_from('<256H', image, 254*512)
        self.assertEqual(summary['free_blocks'], sum(1 for b in range(200) if fat[b] == FAT_FREE))
        self.assertEqual(summary['free_blocks'], 200 - len(SAVE)//512 - 2 - len(GAME)//512)
        # Idempotent: scrubbing the archive again changes nothing.
        self.assertEqual(scrub_card(image)[0], image)

    def test_stale_tokens_in_free_space_and_renamed_login_files(self):
        card = bytearray(fixture_card([('RENAMED', AUTH, 0x33, 0)]))
        # A deleted login save whose blocks were never cleared.
        card[199*512:199*512+len(AUTH)//512*512] = AUTH[:512]
        image, summary = scrub_card(bytes(card))
        self.assertNotIn(AUTH_MARKER, image)
        self.assertEqual(summary['removed'], 2)
        self.assertEqual(image[199*512:200*512], bytes(512))
        renamed = bytearray(fixtures.vms(offset=1))
        renamed[512+48:512+64] = b'DCVMU_AUTH'.ljust(16, b'\0')
        image, summary = scrub_card(card_image([('APPID', bytes(renamed), 0x33, 1), ('KEEP', fixtures.vms(), 0x33, 0)]))
        self.assertEqual(summary['removed'], 1)
        self.assertEqual(directory_names(image), {'KEEP'})

    def test_damaged_entries_are_kept_and_bad_cards_rejected(self):
        card = bytearray(fixture_card())
        # Break TEST_SAVE's chain: point its first block at the system area.
        pos = 253*512
        struct.pack_into('<H', card, pos+2, 254)
        image, summary = scrub_card(bytes(card))
        self.assertEqual(summary['files'][0]['filename'], 'TEST_SAVE')
        self.assertTrue(summary['files'][0]['reason'])
        self.assertEqual(image[pos:pos+32], bytes(card[pos:pos+32]))
        for bad in (b'', bytes(128*1024), b'x'*(128*1024), fixture_card()[:-1]):
            with self.assertRaises(ValueError):
                scrub_card(bad)
        leaked = bytearray(card_image([('KEEP', fixtures.vms(), 0x33, 0)]))
        struct.pack_into('<H', leaked, 255*512+0x50, 0)
        leaked[210*512:210*512+len(AUTH_MARKER)] = AUTH_MARKER
        with self.assertRaises(ValueError):
            scrub_card(bytes(leaked))


class ArchiveServiceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = self.temp.name + '/test.sqlite3'
        self.app = create_app({'TESTING': True, 'DATABASE': self.path})
        self.web = self.app.test_client()
        self.register(self.web, 'tester')
        self.api = self.app.test_client()
        self.auth = self.login('tester')

    def tearDown(self):
        self.temp.cleanup()

    def csrf(self, client, path):
        response = client.get(path)
        self.assertEqual(response.status_code, 200)
        return re.search(r'name="csrf" value="([^"]+)"', response.text)[1]

    def register(self, client, name):
        response = client.post('/register', data={'csrf': self.csrf(client, '/register'), 'username': name,
                                                  'password': 'a-long-test-password', 'email': name + '@example.com'})
        self.assertEqual(response.status_code, 303)

    def login(self, name):
        response = self.api.post('/api/v1/login', data={'username': name, 'password': 'a-long-test-password'})
        self.assertEqual(response.status_code, 200)
        return {'Authorization': 'Bearer ' + response.text.strip()}

    def api_upload(self, card=None, **fields):
        data = {'name': 'Before final boss', 'source': 'VMU A1',
                'image': (io.BytesIO(fixture_card() if card is None else card), 'vmu.bin', 'application/octet-stream')}
        data.update(fields)
        return self.api.post('/api/v1/archives', data=data, headers=self.auth)

    def web_upload(self, card, filename, name='Flycast card'):
        return self.web.post('/archives', data={'csrf': self.csrf(self.web, '/archives'), 'name': name,
                                                'image': (io.BytesIO(card), filename)})

    def test_api_upload_list_download_and_scrubbing(self):
        with patch('app.time.time', return_value=1700000000):
            response = self.api_upload()
        self.assertEqual(response.status_code, 201)
        self.assertEqual(response.text.splitlines()[0], 'OK')
        aid = response.text.splitlines()[1]
        self.assertEqual(response.text.splitlines()[2], 'Before final boss')
        expected = scrub_card(fixture_card())[0]
        with sqlite3.connect(self.path) as db:
            row = db.execute('SELECT name,source,files,free_blocks,data,sha256,created,revision FROM archives').fetchone()
        self.assertEqual(row[:4], ('Before final boss', 'VMU A1', 3, 200 - len(SAVE)//512 - 2 - len(GAME)//512))
        self.assertEqual(row[4], expected)
        self.assertNotIn(AUTH_MARKER, row[4])
        self.assertEqual(row[5], hashlib.sha256(expected).hexdigest())
        self.assertEqual(row[6:], (1700000000, 1))
        listed = self.api.get('/api/v1/archives', headers=self.auth)
        self.assertEqual(listed.status_code, 200)
        lines = listed.text.splitlines()
        self.assertEqual(lines[0], 'MORE\t0')
        self.assertEqual(lines[1].split('\t'), [aid, '1', 'Before%20final%20boss', '1700000000', '3', '131072',
                                                hashlib.sha256(expected).hexdigest(), 'VMU%20A1'])
        download = self.api.get('/api/v1/archives/' + aid + '/download?revision=1', headers=self.auth)
        self.assertEqual(download.status_code, 200)
        self.assertEqual(download.data, expected)
        self.assertEqual(self.api.get('/api/v1/archives/' + aid + '/download?revision=2', headers=self.auth).status_code, 409)
        self.assertEqual(self.api.get('/api/v1/archives/' + aid + '/download?revision=1').status_code, 401)
        self.assertEqual(self.api.get('/api/v1/archives').status_code, 401)
        # An untitled archive is listed under a default title and keeps its datestamp.
        untitled = self.api_upload(name='', source='')
        self.assertEqual(untitled.text.splitlines()[2], 'VMU archive')
        rows = self.api.get('/api/v1/archives', headers=self.auth).text.splitlines()
        self.assertEqual(rows[1].split('\t')[2], 'VMU%20archive')
        self.assertEqual(rows[1].split('\t')[7], '')
        self.assertEqual(self.app.test_client().get('/api/v1/archives').status_code, 401)

    def test_api_upload_validation_and_limits(self):
        for card, mimetype in ((fixture_card()[:-512], 'application/octet-stream'), (fixture_card() + bytes(512), 'application/octet-stream'),
                               (bytes(128*1024), 'application/octet-stream'), (fixture_card(), 'text/plain')):
            self.assertEqual(self.api_upload(image=(io.BytesIO(card), 'vmu.bin', mimetype)).status_code, 400)
        self.assertEqual(self.api_upload(name='x' * 65).status_code, 400)
        self.assertEqual(self.api_upload(name='two\nlines').status_code, 400)
        self.assertEqual(self.api_upload(source='x' * 25).status_code, 400)
        self.assertEqual(self.api.post('/api/v1/archives', data={'name': 'none'}, headers=self.auth).status_code, 400)
        self.assertIn(self.api.post('/api/v1/archives', data={'image': [
            (io.BytesIO(fixture_card()), 'a.bin', 'application/octet-stream'),
            (io.BytesIO(fixture_card()), 'b.bin', 'application/octet-stream')]}, headers=self.auth).status_code, (400, 413))
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM archives').fetchone()[0], 0)
        for number in range(MAX_ARCHIVES):
            self.assertEqual(self.api_upload(name=f'Archive {number}').status_code, 201)
        full = self.api_upload(name='One too many')
        self.assertEqual(full.status_code, 400)
        self.assertIn('Archive limit reached', full.text)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM archives').fetchone()[0], MAX_ARCHIVES)
        pages = [self.api.get('/api/v1/archives?page=%d' % page, headers=self.auth).text.splitlines() for page in (0, 1, 2)]
        self.assertEqual([page[0] for page in pages], ['MORE\t1', 'MORE\t1', 'MORE\t0'])
        self.assertEqual(sum(len(page) - 1 for page in pages), MAX_ARCHIVES)
        self.assertEqual(self.api.get('/api/v1/archives?page=-1', headers=self.auth).status_code, 400)

    def test_web_upload_formats_pages_edit_and_delete(self):
        self.assertEqual(self.app.test_client().get('/archives').status_code, 401)
        self.assertIn('href="/archives"', self.web.get('/account').text)
        empty = self.web.get('/archives')
        self.assertIn('No VMU archives yet', empty.text)
        self.assertIn(f'Using 0 of {MAX_ARCHIVES} archives', empty.text)
        expected = scrub_card(fixture_card())[0]
        ids = []
        for filename, card in (('flycast_vmu_save_A1.bin', fixture_card()), ('card.VMU', fixture_card()),
                               ('nexus.dcm', swap_words(fixture_card()))):
            with self.subTest(filename=filename):
                with patch('app.time.time', return_value=1700000000):
                    response = self.web_upload(card, filename, name='Card ' + filename)
                self.assertEqual(response.status_code, 303, response.text)
                aid = re.fullmatch(r'/archives/(\d+)\?archived=1', response.location)[1]
                ids.append(aid)
                with sqlite3.connect(self.path) as db:
                    self.assertEqual(db.execute('SELECT data,source FROM archives WHERE id=?', (aid,)).fetchone(),
                                     (expected, filename[:24]))
                detail = self.web.get(response.location)
                self.assertEqual(detail.status_code, 200)
                self.assertIn('Archived 3 files', detail.text)
                self.assertIn('Card ' + filename, detail.text)
                for name in ('TEST_SAVE', 'ICONDATA_VMS', 'MINIGAME'):
                    self.assertIn(name, detail.text)
                self.assertNotIn('DCVMU_AUTH', detail.text)
                self.assertIn('VMU mini-game', detail.text)
                self.assertIn('2023-11-14T22:13:20Z', detail.text)
                self.assertIn(f'/archives/{aid}/0/icon.png', detail.text)
                icon = self.web.get(f'/archives/{aid}/0/icon.png')
                self.assertEqual(icon.mimetype, 'image/png')
                self.assertEqual(self.web.get(f'/archives/{aid}/3/icon.png').status_code, 404)
        for name, raw in (('bad.dci', fixture_card()), ('card.zip', fixture_card()), ('short.bin', fixture_card()[:-1]),
                          ('blank.bin', bytes(128*1024))):
            self.assertIn(self.web_upload(raw, name).status_code, (400, 413))
        self.assertEqual(self.web.post('/archives', data={'csrf': self.csrf(self.web, '/archives')}).status_code, 400)
        listing = self.web.get('/archives')
        self.assertEqual(listing.text.count('class="save-card"'), 3)
        self.assertIn('Using 3 of', listing.text)
        aid = ids[0]
        download = self.web.get(f'/archives/{aid}/download')
        self.assertEqual(download.data, expected)
        self.assertIn('filename=Card-flycast-vmu-save-A1-bin-2023-11-14.bin', download.headers['Content-Disposition'])
        detail = f'/archives/{aid}'
        edit = self.web.post(detail + '/edit', data={'csrf': self.csrf(self.web, detail), 'revision': '1', 'name': 'Renamed'})
        self.assertEqual(edit.status_code, 303)
        self.assertEqual(self.web.post(detail + '/edit', data={'csrf': self.csrf(self.web, detail), 'revision': '1', 'name': 'Stale'}).status_code, 409)
        self.assertEqual(self.web.post(detail + '/edit', data={'csrf': self.csrf(self.web, detail), 'revision': '2', 'name': 'x' * 65}).status_code, 400)
        self.assertIn('Renamed', self.web.get(detail).text)
        self.assertIn('Renamed', self.api.get('/api/v1/archives', headers=self.auth).text)
        self.assertEqual(self.api.get(f'/api/v1/archives/{aid}/download?revision=1', headers=self.auth).status_code, 409)
        self.assertEqual(self.api.get(f'/api/v1/archives/{aid}/download?revision=2', headers=self.auth).data, expected)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT data FROM archives WHERE id=?', (aid,)).fetchone()[0], expected)
        confirm = self.web.get(detail + '/delete')
        self.assertIn('Cancel', confirm.text)
        self.assertIn('delete-save-link', self.web.get(detail).text)
        data = {'csrf': self.csrf(self.web, detail), 'revision': '2', 'confirm': 'yes'}
        self.assertEqual(self.web.post(detail + '/delete', data=dict(data, confirm='no')).status_code, 400)
        self.assertEqual(self.web.post(detail + '/delete', data=dict(data, revision='1')).status_code, 409)
        self.assertEqual(self.web.post(detail + '/delete', data=dict(data, csrf='')).status_code, 403)
        deleted = self.web.post(detail + '/delete', data=data)
        self.assertEqual(deleted.status_code, 303)
        self.assertEqual(deleted.location, '/archives')
        for path in (detail, detail + '/download', detail + '/delete', detail + '/0/icon.png'):
            self.assertEqual(self.web.get(path).status_code, 404)
        self.assertEqual(self.api.get(f'/api/v1/archives/{aid}/download?revision=2', headers=self.auth).status_code, 404)
        self.assertEqual(self.web.get('/archives').text.count('class="save-card"'), 2)

    def test_archives_are_owner_only(self):
        aid = self.api_upload().text.splitlines()[1]
        detail = f'/archives/{aid}'
        other = self.app.test_client()
        self.assertEqual(other.get(detail).status_code, 401)
        self.register(other, 'other')
        token = other.post('/api/v1/login', data={'username': 'other', 'password': 'a-long-test-password'}).text.strip()
        headers = {'Authorization': 'Bearer ' + token}
        for path in (detail, detail + '/download', detail + '/delete', detail + '/0/icon.png'):
            self.assertEqual(other.get(path).status_code, 404)
        csrf = self.csrf(other, '/archives')
        self.assertEqual(other.post(detail + '/edit', data={'csrf': csrf, 'revision': '1', 'name': 'Hijacked'}).status_code, 404)
        self.assertEqual(other.post(detail + '/delete', data={'csrf': csrf, 'revision': '1', 'confirm': 'yes'}).status_code, 404)
        self.assertEqual(other.get(f'/api/v1/archives/{aid}/download?revision=1', headers=headers).status_code, 404)
        self.assertEqual(other.get('/api/v1/archives', headers=headers).text, 'MORE\t0\n')
        self.assertNotIn('Before final boss', other.get('/archives').text)
        self.assertNotIn('Before final boss', other.get('/').text)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT name,revision FROM archives').fetchone(), ('Before final boss', 1))

    def test_upload_rate_limit_is_shared_with_saves(self):
        with sqlite3.connect(self.path) as db:
            uid = db.execute("SELECT id FROM users WHERE username='tester'").fetchone()[0]
            db.execute('INSERT INTO limits VALUES (?,60,9999999999)', ('upload:' + str(uid),))
        self.assertEqual(self.api_upload().status_code, 429)
        self.assertEqual(self.web_upload(fixture_card(), 'card.bin').status_code, 429)


if __name__ == '__main__':
    unittest.main()
