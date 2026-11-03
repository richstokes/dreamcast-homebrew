import io
import re
import tempfile
import unittest

import dreambox
from app import create_app


class DreamboxTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.app = create_app({'TESTING': True, 'DATABASE': self.temp.name + '/test.sqlite3'})
        self.web = self.app.test_client()
        r = self.web.post('/register', data={'csrf': self.csrf('/register'), 'username': 'tester',
                                             'password': 'a-long-test-password', 'email': 'tester@example.com'})
        self.assertEqual(r.status_code, 303)
        self.console = self.app.test_client()
        self.auth = {'Authorization': 'Bearer ' + self.link()}

    def tearDown(self):
        self.temp.cleanup()

    def csrf(self, path):
        return re.search(r'name="csrf" value="([^"]+)"', self.web.get(path).text)[1]

    def link(self):
        r = self.console.post('/api/v1/drive/pair')
        self.assertEqual(r.status_code, 200)
        device, code, seconds = r.text.split('\n')[:3]
        self.assertRegex(code, r'^[A-Z2-9]{4}-[A-Z2-9]{4}$')
        self.assertEqual(seconds, '900')
        self.assertEqual(self.console.post('/api/v1/drive/pair/claim', query_string={'device': device}).status_code, 202)
        r = self.web.post('/account/archicast/link', data={'csrf': self.csrf('/account/archicast'), 'code': code.lower()})
        self.assertEqual(r.status_code, 303)
        r = self.console.post('/api/v1/drive/pair/claim', query_string={'device': device})
        self.assertEqual(r.status_code, 200)
        # A code links once
        self.assertEqual(self.console.post('/api/v1/drive/pair/claim', query_string={'device': device}).status_code, 404)
        return r.text.strip()

    def call(self, verb, endpoint, data=None, **args):
        return getattr(self.console, verb)('/api/v1/drive/' + endpoint, query_string=args, data=data, headers=self.auth)

    def save(self, path, data, load='FFFFFF00', exec_='12345678', **extra):
        for block in range(0, max(len(data), 1), dreambox.BLOCK):
            r = self.call('post', 'write', data[block:block + dreambox.BLOCK], path=path,
                          block=block // dreambox.BLOCK, fresh='1' if block == 0 else '0')
            self.assertEqual(r.status_code, 200, r.text)
        return self.call('post', 'commit', path=path, size=len(data), load=load, exec=exec_, staged='1', **extra)

    def test_tokens_and_codes(self):
        self.assertEqual(self.console.get('/api/v1/drive/list').status_code, 401)
        self.assertEqual(self.console.get('/api/v1/drive/list', headers={'Authorization': 'Bearer nope'}).status_code, 401)
        # Web logins and save API tokens are not drive tokens
        r = self.console.post('/api/v1/login', data={'username': 'tester', 'password': 'a-long-test-password'})
        self.assertEqual(self.console.get('/api/v1/drive/list', headers={'Authorization': 'Bearer ' + r.text.strip()}).status_code, 401)
        r = self.web.post('/account/archicast/link', data={'csrf': self.csrf('/account/archicast'), 'code': 'AAAA-AAAA'})
        self.assertEqual(r.status_code, 400)
        page = self.web.get('/account/archicast').text
        link = re.search(r'name="link" value="([^"]+)"', page)[1]
        self.web.post('/account/archicast/unlink', data={'csrf': self.csrf('/account/archicast'), 'link': link})
        self.assertEqual(self.call('get', 'list').status_code, 401)

    def test_files_round_trip_through_blocks(self):
        data = bytes(range(256)) * 300  # 76800 bytes, three blocks
        self.assertEqual(self.save('Picture', data, load='FFFAFF12').status_code, 200)
        self.assertEqual(self.call('get', 'info', path='$.Picture').text,
                         '1 FFFAFF12 12345678 76800 3 Picture\n')
        r = self.call('get', 'read', path='Picture', offset=dreambox.BLOCK - 10, length=dreambox.BLOCK)
        self.assertEqual(r.data, data[dreambox.BLOCK - 10:2 * dreambox.BLOCK - 10])
        # Short at the end of the file
        self.assertEqual(self.call('get', 'read', path='Picture', offset=76000, length=4096).data, data[76000:])
        self.assertEqual(self.call('get', 'list').text, '1 FFFAFF12 12345678 76800 3 Picture\n')

    def test_update_keeps_unwritten_blocks(self):
        data = b'a' * (2 * dreambox.BLOCK + 5)
        self.save('Doc', data)
        patch = b'b' * dreambox.BLOCK
        self.call('post', 'write', patch, path='Doc', block=1, fresh='1')
        # Reads through staging see the new block and the old ones around it
        r = self.call('get', 'read', path='Doc', offset=dreambox.BLOCK - 2, length=4, staged='1')
        self.assertEqual(r.data, b'aabb')
        self.assertEqual(self.call('get', 'read', path='Doc', offset=0, length=2).data, b'aa')
        r = self.call('post', 'commit', path='Doc', size=len(data) + 3, load='FFFFFF00', exec='0', staged='1', keep='1')
        self.assertEqual(r.status_code, 200)
        whole = b''.join(self.call('get', 'read', path='Doc', offset=o, length=dreambox.BLOCK).data
                         for o in range(0, len(data) + 3, dreambox.BLOCK))
        self.assertEqual(whole, b'a' * dreambox.BLOCK + patch + b'aaaaa\0\0\0')
        # Without keep, unwritten blocks are zero; staged=0 ignores what was staged
        self.call('post', 'write', b'zz', path='Doc', block=0, fresh='1')
        self.call('post', 'commit', path='Doc', size=4, load='FFFFFF00', exec='0', staged='0')
        self.assertEqual(self.call('get', 'read', path='Doc', offset=0, length=10).data, b'\0\0\0\0')

    def test_directories_rename_delete_and_stamps(self):
        self.assertEqual(self.call('post', 'mkdir', path='Work').status_code, 200)
        self.assertEqual(self.call('post', 'mkdir', path='Work').status_code, 200)
        self.assertEqual(self.save('Work.Letter/txt', b'hello').status_code, 200)
        self.assertEqual(self.save('Nope.Letter', b'x').status_code, 404)
        self.assertEqual(self.call('post', 'delete', path='Work').status_code, 409)
        self.assertEqual(self.call('post', 'rename', **{'from': 'Work.Letter/txt', 'to': 'Letter'}).status_code, 200)
        self.assertEqual(self.call('post', 'rename', **{'from': 'Work', 'to': 'Work.Inner'}).status_code, 400)
        self.assertEqual(self.call('post', 'stamp', path='letter', load='FFFFFF01', attr='13').status_code, 200)
        self.assertEqual(self.call('get', 'info', path='LETTER').text, '1 FFFFFF01 12345678 5 13 Letter\n')
        self.assertEqual(self.call('post', 'delete', path='Letter').text, '1 FFFFFF01 12345678 5 13 Letter\n')
        self.assertEqual(self.call('post', 'delete', path='Letter').text, '0\n')
        self.assertTrue(self.call('get', 'list').text.startswith('2 FFFFFD'))
        self.assertEqual(self.call('get', 'info', path='Bad name').status_code, 400)
        self.assertEqual(self.call('post', 'mkdir', path='').status_code, 400)

    def test_limits(self):
        self.assertEqual(self.call('post', 'write', b'x' * (dreambox.BLOCK + 1), path='Big', block=0).status_code, 413)
        r = self.call('post', 'commit', path='Big', size=dreambox.MAX_FILE + 1, load='0', exec='0')
        self.assertEqual(r.status_code, 400)
        original = dreambox.QUOTA
        dreambox.QUOTA = 100
        try:
            self.assertEqual(self.save('A', b'x' * 60).status_code, 200)
            self.assertEqual(self.save('B', b'x' * 60).status_code, 413)
            self.assertEqual(self.save('A', b'x' * 90).status_code, 200)  # replacing frees the old size
        finally:
            dreambox.QUOTA = original

    def test_website_upload_download_and_names(self):
        token = self.csrf('/account/archicast')
        r = self.web.post('/account/archicast/upload', data={'csrf': token, 'path': '',
                          'file': (io.BytesIO(b'drawing'), 'My Picture,aff')}, content_type='multipart/form-data')
        self.assertEqual(r.status_code, 303)
        self.web.post('/account/archicast/upload', data={'csrf': token, 'path': '',
                      'file': (io.BytesIO(b'text'), 'notes.txt')}, content_type='multipart/form-data')
        listing = self.call('get', 'list').text.splitlines()
        self.assertRegex(listing[0], r'^1 FFFAFF.. [0-9A-F]{8} 7 3 My_Picture$')
        self.assertRegex(listing[1], r'^1 FFFFFF.. [0-9A-F]{8} 4 3 notes/txt$')
        r = self.web.get('/account/archicast/download', query_string={'path': 'My_Picture'})
        self.assertEqual(r.data, b'drawing')
        self.assertIn('My_Picture,aff', r.headers['Content-Disposition'])
        r = self.web.get('/account/archicast/download', query_string={'path': 'notes/txt'})
        self.assertIn('notes.txt', r.headers['Content-Disposition'])
        self.web.post('/account/archicast/mkdir', data={'csrf': token, 'path': '', 'name': 'Docs'})
        page = self.web.get('/account/archicast').text
        self.assertIn('DrawFile', page)
        self.assertIn('href="/account/archicast?path=Docs"', page)
        self.web.post('/account/archicast/delete', data={'csrf': token, 'path': 'Docs'})
        self.assertNotIn('Docs', self.call('get', 'list').text)
        self.assertIn('href="/account/archicast"', self.web.get('/account').text)

    def test_names(self):
        self.assertEqual(dreambox.web_name('C:\\x\\report.final.pdf'), ('report/final/pdf', 0xADF))
        self.assertEqual(dreambox.web_name('!Boot,feb'), ('!Boot', 0xFEB))
        self.assertEqual(dreambox.web_name('a b.unknown'), ('a_b/unknown', 0xFFD))
        self.assertEqual(dreambox.download_name('Letter', 0xFFFFFF00), 'Letter.txt')
        self.assertEqual(dreambox.download_name('Pic', 0xFFFAFF00), 'Pic,aff')
        self.assertEqual(dreambox.download_name('Code', 0x8000), 'Code')


if __name__ == '__main__':
    unittest.main()
