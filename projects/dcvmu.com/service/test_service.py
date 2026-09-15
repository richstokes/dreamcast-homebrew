import io
import struct
import binascii
import re
import sqlite3
import tempfile
import unittest
import zlib
from html import unescape
from urllib.parse import parse_qs, urlsplit
from concurrent.futures import ThreadPoolExecutor
from app import create_app

def vms(payload=b'x'*256, icons=0, eye=0, offset=0, palette=None, frames=None):
    eye_bytes=(0,8064,4544,2048)[eye]
    data=bytearray(128+icons*512+eye_bytes+len(payload))
    data[:16]=b'Test save       '
    struct.pack_into('<HHHHI',data,64,icons,1,eye,0,len(payload))
    if palette is not None:
        struct.pack_into('<16H', data, 96, *palette)
    if frames is not None:
        assert len(frames) == icons * 512
        data[128:128+len(frames)] = frames
    data[-len(payload):]=payload
    struct.pack_into('<H',data,70,binascii.crc_hqx(data,0))
    data=bytes(offset*512)+bytes(data)
    return data+bytes((-len(data))%512)


class IconTest(unittest.TestCase):
    def test_png_pixels_palette_alpha_and_first_frame(self):
        from vmu_validation import first_icon_png
        palette = [0xF123, 0x8ABC, 0x0456, 0xFFFF] + [0] * 12
        first = b'\x01\x23' + bytes(14) + b'\x32' * 16 + bytes(480)
        for icons in (1, 2, 3):
            raw = vms(icons=icons, palette=palette,
                      frames=first + b'\xFF' * ((icons-1)*512))
            png = first_icon_png(raw)
            self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
            chunks = {}
            cursor = 8
            while cursor < len(png):
                size = struct.unpack_from('>I', png, cursor)[0]
                kind = png[cursor+4:cursor+8]
                body = png[cursor+8:cursor+8+size]
                crc = struct.unpack_from('>I', png, cursor+8+size)[0]
                self.assertEqual(crc, binascii.crc32(kind+body))
                chunks[kind] = body
                cursor += size+12
            self.assertEqual(set(chunks), {b'IHDR', b'IDAT', b'IEND'})
            self.assertEqual(struct.unpack('>IIBBBBB', chunks[b'IHDR']), (32,32,8,6,0,0,0))
            pixels = zlib.decompress(chunks[b'IDAT'])
            self.assertEqual(len(pixels), 32 * 129)
            self.assertEqual(pixels[:17], bytes([0, 17,34,51,255, 170,187,204,136,
                                               68,85,102,0, 255,255,255,255]))
            self.assertEqual(pixels[129:138], bytes([0, 255,255,255,255, 68,85,102,0]))
            self.assertTrue(all(pixels[y*129] == 0 for y in range(32)))

    def test_missing_invalid_and_truncated_icon(self):
        from vmu_validation import first_icon_png, has_vms_icon
        for raw in (b'', bytes(128), vms(), vms(icons=4), vms(icons=1)[:639]):
            self.assertFalse(has_vms_icon(raw))
            self.assertIsNone(first_icon_png(raw))

class ServiceTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.path=self.temp.name+'/test.sqlite3'
        self.app=create_app({'TESTING':True,'DATABASE':self.path})
        self.web=self.app.test_client()
        self.register('tester')
        self.api=self.app.test_client()
        r=self.api.post('/api/v1/login',data={'username':'tester','password':'a-long-test-password'})
        self.assertEqual(r.status_code,200)
        self.auth={'Authorization':'Bearer '+r.text.strip()}
    def tearDown(self): self.temp.cleanup()
    def csrf(self,path):
        r=self.web.get(path)
        self.assertEqual(r.status_code,200)
        return re.search(r'name="csrf" value="([^"]+)"',r.text)[1]
    def register(self,name):
        r=self.web.post('/register',data={'csrf':self.csrf('/register'),'username':name,'password':'a-long-test-password','email':name+'@example.com'})
        self.assertEqual(r.status_code,303)
    def upload(self,**overrides):
        data={'name':'My save','filename':'TEST_SAVE','game':'Test game','notes':'<script>x</script>','private':'0','save':(io.BytesIO(vms()),'test.vms','application/octet-stream')}
        data.update(overrides)
        return self.api.post('/api/v1/saves',data=data,headers=self.auth)

    def test_web_sorting_filters_and_pagination(self):
        records = []
        with sqlite3.connect(self.path) as db:
            owner = db.execute("SELECT id FROM users WHERE username='tester'").fetchone()[0]
            other = db.execute('INSERT INTO users(username,email,password_hash,created) VALUES (?,?,?,?)',
                               ('Alpha', 'alpha@example.com', 'unused', 1)).lastrowid
            for number in range(47):
                name = ('alpha' if number % 2 else 'Beta') + str(number % 3)
                user = 'tester' if number < 24 else 'Alpha'
                uploaded = None if number % 7 == 0 else 100 + number % 5
                created = 50 + number
                game = 'Other' if number == 46 else 'Test game'
                private = int(number == 45)
                sid = db.execute('''INSERT INTO saves(user_id,name,filename,game,notes,private,data,
                    sha256,created,updated,uploaded_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)''',
                    (owner if user == 'tester' else other, name + f' {number:02}', 'TEST_SAVE',
                     game, '', private, vms(), 'fixture', created, 1000-number, uploaded)).lastrowid
                records.append(dict(id=sid, name=name + f' {number:02}', user=user,
                                    date=uploaded if uploaded is not None else created,
                                    game=game, private=private))
        def ids(response):
            self.assertEqual(response.status_code, 200)
            return [int(sid) for sid in re.findall(r'(?:<h2><a|<a class="list-save-name") href="/saves/(\d+)"', response.text)]
        visible = [r for r in records if not r['private']]
        expected = {
            'uploaded_desc': sorted(visible, key=lambda r: (r['date'], r['id']), reverse=True),
            'uploaded_asc': sorted(visible, key=lambda r: (r['date'], r['id'])),
            'name_asc': sorted(visible, key=lambda r: (r['name'].lower(), r['id'])),
            'name_desc': sorted(visible, key=lambda r: (r['name'].lower(), r['id']), reverse=True),
            'user_asc': sorted(visible, key=lambda r: (r['user'].lower(), r['name'].lower(), r['id'])),
            'user_desc': sorted(visible, key=lambda r: (r['user'] == 'Alpha', r['name'].lower(), r['id'])),
        }
        for view in ('cards', 'list'):
            for sort, ordered in expected.items():
                with self.subTest(sort=sort, view=view):
                    pages = [self.web.get('/', query_string={'sort': sort, 'view': view, 'page': page}) for page in (1, 2, 3)]
                    self.assertEqual([sid for page in pages for sid in ids(page)], [r['id'] for r in ordered])
                    self.assertIn(f'value="{sort}" selected', pages[0].text)
                    self.assertIn(f'value="{view}" selected', pages[0].text)
                    self.assertEqual('<table class="save-table"' in pages[0].text, view == 'list')
                    for response in pages:
                        for url in re.findall(r'href="([^"]+)">(?:Next|Previous) page', response.text):
                            params = parse_qs(urlsplit(unescape(url)).query)
                            self.assertEqual(params['sort'], [sort])
                            self.assertEqual(params['view'], [view])
        for sort in (None, 'invalid', 's.name; DROP TABLE saves'):
            response = self.web.get('/', query_string={} if sort is None else {'sort': sort})
            self.assertEqual(ids(response), [r['id'] for r in expected['uploaded_desc'][:20]])
        query = {'sort': 'name_asc', 'user': 'TeStEr', 'game': 'Test game', 'view': 'list'}
        first = self.web.get('/', query_string=query)
        next_url = unescape(re.search(r'href="([^"]+)">Next page', first.text)[1])
        self.assertEqual(parse_qs(urlsplit(next_url).query), {**{k: [v] for k, v in query.items()}, 'page': ['2']})
        self.assertEqual(ids(first) + ids(self.web.get(next_url)),
                         [r['id'] for r in expected['name_asc'] if r['user'] == 'tester' and r['game'] == 'Test game'])

    def test_browse_view_fallback_empty_and_console(self):
        empty = self.web.get('/?view=list&sort=name_asc&user=nobody')
        self.assertIn('No matching saves', empty.text)
        self.assertNotIn('<table', empty.text)
        self.assertEqual(self.upload().status_code, 201)
        fallback = self.web.get('/?view=invalid')
        self.assertIn('value="cards" selected', fallback.text)
        self.assertIn('class="save-card"', fallback.text)
        desktop = self.web.get('/?view=list').text
        self.assertIn('/static/browse.js', desktop)
        self.assertIn('data-local-time', desktop)
        self.assertIn('value="Apply"', desktop)
        for agent in ('DreamKey', 'DreamPassport', 'Dreamcast'):
            console = self.web.get('/?view=list', headers={'User-Agent': agent}).text
            self.assertNotIn('/static/browse.js', console)
            self.assertIn('<table class="save-table"', console)
            self.assertIn('value="Apply"', console)

    def test_download_icon_ranges(self):
        for offset in (0, 1):
            with self.subTest(offset=offset):
                save = vms(icons=3, offset=offset)
                result = self.upload(name='Icon ' + str(offset), private='1',
                    header_offset=str(offset), save=(io.BytesIO(save), 'icon.vms', 'application/octet-stream'))
                self.assertEqual(result.status_code, 201)
                sid = result.text.splitlines()[1]
                url = '/api/v1/saves/' + sid + '/download'
                start = offset * 512
                headers = dict(self.auth, Range=f'bytes={start}-{start+639}')
                preview = self.api.get(url + '?revision=1', headers=headers)
                self.assertEqual(preview.status_code, 206)
                self.assertEqual(preview.data, save[start:start+640])
                self.assertEqual(preview.headers['Content-Range'], f'bytes {start}-{start+639}/{len(save)}')
                self.assertEqual(self.api.get(url + '?revision=0', headers=headers).status_code, 409)
                self.assertEqual(self.api.get(url + '?revision=1',
                    headers={'Range': headers['Range']}).status_code, 401)
                self.register('reader' + str(offset))
                reader = self.app.test_client()
                login = reader.post('/api/v1/login', data={
                    'username': 'reader' + str(offset), 'password': 'a-long-test-password'})
                self.assertEqual(reader.get(url + '?revision=1', headers={
                    'Authorization': 'Bearer ' + login.text.strip(),
                    'Range': headers['Range']}).status_code, 404)

    def test_icon_visibility_offsets_and_replacement(self):
        from vmu_validation import first_icon_png
        guest = self.app.test_client()
        for offset in (0, 1):
            raw = vms(icons=3, offset=offset, eye=2, palette=[0xF123]*16,
                      frames=b'\x00'*512 + b'\x11'*1024)
            response = self.upload(name='Icon '+str(offset), private=str(offset),
                save=(io.BytesIO(raw), 'test.vms', 'application/octet-stream'))
            self.assertEqual(response.status_code, 201)
            sid = response.text.splitlines()[1]
            detail = '/saves/'+sid
            url = detail+'/icon.png'
            icon = self.web.get(url)
            self.assertEqual(icon.status_code, 200)
            self.assertEqual(icon.mimetype, 'image/png')
            self.assertEqual(icon.headers['Cache-Control'], 'no-store')
            self.assertEqual(icon.headers['X-Content-Type-Options'], 'nosniff')
            self.assertEqual(icon.data, first_icon_png(raw[offset*512:]))
            self.assertEqual(self.web.get(detail+'/download').data, raw)
            for path in (detail, '/account'):
                self.assertIn('src="'+url+'"', self.web.get(path).text)
            self.assertEqual(guest.get(url).status_code, 404 if offset else 200)
            self.assertEqual('src="'+url+'"' in guest.get('/').text, not offset)
            if not offset:
                updated = vms(icons=1, palette=[0xFABC]*16)
                response = self.upload(name='Icon 0', mode='replace', revision='1',
                    private='1', save=(io.BytesIO(updated), 'test.vms', 'application/octet-stream'))
                self.assertEqual(response.status_code, 201)
                self.assertEqual(guest.get(url).status_code, 404)
                self.assertEqual(self.web.get(url).data, first_icon_png(updated))
            else:
                self.register('other')
                self.assertEqual(self.web.get(url).status_code, 404)

    def test_icon_absent_deleted_and_corrupt(self):
        sid = self.upload().text.splitlines()[1]
        detail = '/saves/'+sid
        url = detail+'/icon.png'
        self.assertEqual(self.web.get(url).status_code, 404)
        for path in ('/', '/account', detail):
            self.assertNotIn('class="save-icon"', self.web.get(path).text)
        with sqlite3.connect(self.path) as db:
            db.execute('UPDATE saves SET data=? WHERE id=?', (bytes(1024), sid))
        self.assertEqual(self.web.get(url).status_code, 404)
        self.assertEqual(self.web.post(detail+'/delete', data={
            'csrf':self.csrf(detail), 'revision':'1', 'confirm':'yes'}).status_code, 303)
        self.assertEqual(self.web.get(url).status_code, 404)
        self.assertEqual(self.web.get('/saves/999999/icon.png').status_code, 404)
    def test_game_identity_is_fixed(self):
        sid = self.upload(game='Forged game').text.splitlines()[1]
        detail = '/saves/' + sid
        self.assertNotIn('name="game"', self.web.get(detail).text)
        for revision, extra in ((1, {}), (2, {'game': 'Forged edit'})):
            response = self.web.post(detail + '/edit', data={
                'csrf': self.csrf(detail), 'revision': str(revision),
                'notes': 'Updated notes', 'private': '1', **extra})
            self.assertEqual(response.status_code, 303)
            with sqlite3.connect(self.path) as db:
                self.assertEqual(db.execute('SELECT game,notes,private FROM saves WHERE id=?',
                    (sid,)).fetchone(), ('TEST_SAVE', 'Updated notes', 1))

    def test_title_edits_and_id_replacement(self):
        first = self.upload(name='Before final boss', filename='CRAZYTAXI_DC', match='filename')
        self.assertEqual(first.status_code, 201)
        sid = first.text.splitlines()[1]
        detail = '/saves/' + sid
        with sqlite3.connect(self.path) as db:
            before = db.execute('SELECT data,sha256,filename,game,uploaded_at FROM saves WHERE id=?', (sid,)).fetchone()
        renamed = self.web.post(detail + '/edit', data={'csrf': self.csrf(detail), 'revision': '1',
            'name': 'Crazy Taxi - Personal best', 'notes': 'My record', 'private': '1', 'game': 'Forged'})
        self.assertEqual(renamed.status_code, 303)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT data,sha256,filename,game,uploaded_at FROM saves WHERE id=?', (sid,)).fetchone(), before)
        self.assertEqual(before[3], 'Crazy Taxi')
        self.assertEqual(self.upload(name='Different title', filename='CRAZYTAXI_DC', match='filename').text, 'MATCHES\n')
        listed = self.api.get('/api/v1/saves?filename=CRAZYTAXI_DC', headers=self.auth)
        self.assertEqual(len(listed.text.splitlines()), 2)
        self.assertIn('Crazy%20Taxi%20-%20Personal%20best', listed.text)
        self.assertEqual(self.upload(match='filename', mode='replace', save_id=sid, revision='1', filename='CRAZYTAXI_DC').status_code, 409)
        self.assertEqual(self.upload(match='filename', mode='replace', save_id=sid, revision='2', filename='OTHER_SAVE').status_code, 400)
        replaced = self.upload(name='Should not replace title', match='filename', mode='replace', save_id=sid,
            revision='2', filename='CRAZYTAXI_DC', save=(io.BytesIO(vms(payload=b'new progress')), 'new.vms', 'application/octet-stream'))
        self.assertEqual(replaced.status_code, 201)
        self.assertEqual(replaced.text.splitlines()[1:3], [sid, 'Crazy Taxi - Personal best'])
        self.assertEqual(self.web.get(detail+'/download').data, vms(payload=b'new progress'))
        rename = '/api/v1/saves/' + sid + '/rename'
        self.assertEqual(self.api.post(rename, headers=self.auth, data={'name': 'Final title', 'revision': '3'}).status_code, 200)
        self.assertEqual(self.api.post(rename, headers=self.auth, data={'name': 'Stale', 'revision': '3'}).status_code, 409)
        for invalid in ('', 'x'*65, 'two\nlines', 'two\tcolumns'):
            self.assertEqual(self.api.post(rename, headers=self.auth, data={'name': invalid, 'revision': '4'}).status_code, 400)
        self.assertEqual(self.app.test_client().post(rename, data={'name': 'No auth', 'revision': '4'}).status_code, 401)
        self.register('other')
        login = self.web.post('/api/v1/login', data={'username':'other','password':'a-long-test-password'})
        other = {'Authorization':'Bearer '+login.text.strip()}
        self.assertEqual(self.api.post(rename, headers=other, data={'name': 'Not mine', 'revision': '4'}).status_code, 404)
        self.assertEqual(self.api.post('/api/v1/saves', headers=other, data={
            'name':'Not mine','filename':'CRAZYTAXI_DC','match':'filename','mode':'replace',
            'save_id':sid,'revision':'4','save':(io.BytesIO(vms()),'save.vms','application/octet-stream')}).status_code, 404)

    def test_filename_matches_titles_and_legacy_clients(self):
        sid = self.upload(name='TEST_SAVE').text.splitlines()[1]
        rename = '/api/v1/saves/'+sid+'/rename'
        self.assertEqual(self.api.post(rename, headers=self.auth, data={'name':'Renamed','revision':'1'}).status_code,200)
        self.assertEqual(self.upload(name='TEST_SAVE').text, 'CONFLICT\n2\n')
        self.assertEqual(self.upload(name='TEST_SAVE',mode='replace',revision='2').text.splitlines()[2], 'Renamed')
        # Explicit Keep both works despite identical titles and VMU filenames.
        for i in range(8):
            self.assertEqual(self.upload(name='Renamed', match='filename', mode='keep').status_code,201)
        self.assertEqual(self.upload(name='TEST_SAVE').status_code,409)  # Old client cannot choose among backups.
        pages = [self.api.get('/api/v1/saves', headers=self.auth,
                    query_string={'filename':'TEST_SAVE','page':i}).text.splitlines() for i in (0,1)]
        self.assertEqual([p[0] for p in pages], ['MORE\t1','MORE\t0'])
        self.assertEqual(len(pages[0])+len(pages[1])-2,9)
        self.assertEqual(self.api.get('/api/v1/saves?filename=OTHER',headers=self.auth).text,'MORE\t0\n')
        self.assertEqual(self.api.get('/api/v1/saves?scope=public&filename=TEST_SAVE',headers=self.auth).status_code,400)
        self.assertEqual(self.api.post(rename, headers=self.auth, data={'name':'Renamed (2)','revision':'3'}).status_code,409)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT name,revision FROM saves WHERE id=?',(sid,)).fetchone(),('Renamed',3))

    def test_catalog_preserves_curated_game_and_title(self):
        sid = self.upload(name='Hand picked title',filename='POWSTONE_DAT').text.splitlines()[1]
        legacy = self.upload(name='Personal best',filename='CRAZYTAXI_DC').text.splitlines()[1]
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT game FROM saves WHERE id=?',(sid,)).fetchone()[0],'Power Stone')
            db.execute('UPDATE saves SET game=? WHERE id=?',('Curated label',sid))
            db.execute('UPDATE saves SET game=filename WHERE id=?',(legacy,))
            before = db.execute('SELECT name,filename,data,sha256,revision,uploaded_at,header_offset FROM saves WHERE id=?',(legacy,)).fetchone()
            db.execute('PRAGMA user_version=0')
        create_app({'TESTING':True,'DATABASE':self.path})
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT name,game FROM saves WHERE id=?',(sid,)).fetchone(),('Hand picked title','Curated label'))
            self.assertEqual(db.execute('SELECT game FROM saves WHERE id=?',(legacy,)).fetchone()[0],'Crazy Taxi')
            self.assertEqual(db.execute('SELECT name,filename,data,sha256,revision,uploaded_at,header_offset FROM saves WHERE id=?',(legacy,)).fetchone(),before)
            self.assertEqual(db.execute('PRAGMA user_version').fetchone()[0],1)

    def test_upload_game_comes_from_header(self):
        for offset in (0, 1):
            data = bytearray(vms(offset=offset))
            start = offset * 512
            data[start+16:start+48] = b'Actual game'.ljust(32, b'\0')
            data[start+70:start+72] = b'\0\0'
            struct.pack_into('<H', data, start+70, binascii.crc_hqx(data[start:start+384], 0))
            result = self.upload(name='Header ' + str(offset), game='Forged game',
                save=(io.BytesIO(data), 'test.vms', 'application/octet-stream'))
            self.assertEqual(result.status_code, 201)
            with sqlite3.connect(self.path) as db:
                self.assertEqual(db.execute('SELECT game FROM saves WHERE id=?',
                    (result.text.splitlines()[1],)).fetchone()[0], 'Actual game')

    def test_delete_save(self):
        for private in ('0','1'):
            sid=self.upload(name='Delete '+private,private=private).text.splitlines()[1]
            detail='/saves/'+sid
            url=detail+'/delete'
            self.assertIn('delete-save-link',self.web.get(detail).text)
            self.assertIn('Cancel',self.web.get(url).text)
            data={'csrf':self.csrf(url),'revision':'1','confirm':'yes'}
            self.assertEqual(self.web.get(detail+'/download').status_code,200)
            self.assertEqual(self.web.post(url,data=dict(data,confirm='no')).status_code,400)
            self.assertEqual(self.web.post(url,data=dict(data,csrf='')).status_code,403)
            self.assertEqual(self.web.post(url,data=data,headers={'Origin':'https://evil.example'}).status_code,403)
            self.assertEqual(self.web.post(detail+'/edit',data=dict(data,game='Updated')).status_code,303)
            self.assertEqual(self.web.post(url,data=data).status_code,409)
            self.assertEqual(self.web.get(detail+'/download').status_code,200)
            result=self.web.post(url,data=dict(data,revision='2'))
            self.assertEqual(result.status_code,303)
            self.assertEqual(result.location,'/account')
            for path in (detail,detail+'/download',url):
                self.assertEqual(self.web.get(path).status_code,404)
            self.assertEqual(self.api.get('/api/v1/saves/'+sid+'/download?revision=2',headers=self.auth).status_code,404)
            self.assertNotIn('Delete '+private,self.web.get('/account').text)
            self.assertNotIn('Delete '+private,self.web.get('/').text)
            self.assertEqual(self.web.post(url,data=dict(data,revision='2')).status_code,404)

    def test_delete_save_owner_only(self):
        ids=[self.upload(name='Owner '+private,private=private).text.splitlines()[1] for private in ('0','1')]
        guest=self.app.test_client()
        self.assertNotIn('delete-save-link',guest.get('/saves/'+ids[0]).text)
        for sid in ids:
            self.assertNotEqual(guest.post('/saves/'+sid+'/delete',data={'confirm':'yes','revision':'1'}).status_code,303)
        self.register('other')
        csrf=self.csrf('/account')
        self.assertNotIn('delete-save-link',self.web.get('/saves/'+ids[0]).text)
        for sid in ids:
            url='/saves/'+sid+'/delete'
            self.assertEqual(self.web.get(url).status_code,404)
            self.assertEqual(self.web.post(url,data={'csrf':csrf,'confirm':'yes','revision':'1'}).status_code,404)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM saves').fetchone()[0],2)

    def test_remembered_login_and_auth_save_rejection(self):
        r=self.api.post('/api/v1/login',data={'username':'tester','password':'a-long-test-password','remember':'1'})
        headers={'Authorization':'Bearer '+r.text.strip()}
        self.assertEqual(self.api.get('/api/v1/me',headers=headers).text,'tester\n')
        import time,hashlib
        with sqlite3.connect(self.path) as db:
            expires=db.execute('SELECT expires FROM sessions WHERE hash=?',(hashlib.sha256(r.text.strip().encode()).hexdigest(),)).fetchone()[0]
        self.assertGreater(expires,time.time()+89*86400)
        self.assertEqual(self.upload(filename='DCVMU_AUTH').status_code,400)
        for offset,marker in [(48,b'DCVMU_AUTH'),(128,b'DCVMU-AUTH-V1')]:
            raw=bytearray(vms());raw[offset:offset+len(marker)]=marker
            raw[70:72]=b'\0\0';struct.pack_into('<H',raw,70,binascii.crc_hqx(raw[:384],0))
            self.assertEqual(self.upload(filename='RENAMED',save=(io.BytesIO(raw),'save.vms','application/octet-stream')).status_code,400)
        self.assertEqual(self.api.post('/api/v1/logout',headers=headers).status_code,200)
        self.assertEqual(self.api.get('/api/v1/me',headers=headers).status_code,401)

    def test_download_api_permissions_pagination_and_revision(self):
        from urllib.parse import unquote
        raw=vms(offset=1)
        r=self.upload(name='Private download',private='1',header_offset='1',save=(io.BytesIO(raw),'test.vms','application/octet-stream'))
        self.assertEqual(r.status_code,201);sid=r.text.splitlines()[1]
        listing=self.api.get('/api/v1/saves?scope=mine',headers=self.auth)
        self.assertEqual(listing.status_code,200)
        columns=[unquote(c) for c in listing.text.splitlines()[1].split('\t')]
        self.assertEqual(columns[0],sid);self.assertEqual(columns[8],'1')
        self.assertNotIn(sid+'\t',self.api.get('/api/v1/saves?scope=public',headers=self.auth).text)
        url='/api/v1/saves/'+sid+'/download'
        self.assertEqual(self.api.get(url+'?revision=1',headers=self.auth).data,raw)
        partial=self.api.get(url+'?revision=1',headers=dict(self.auth,Range='bytes=0-511'))
        self.assertEqual(partial.status_code,206);self.assertEqual(partial.data,raw[:512])
        self.assertEqual(self.api.get(url+'?revision=2',headers=self.auth).status_code,409)
        self.assertEqual(self.app.test_client().get(url+'?revision=1').status_code,401)
        self.register('reader')
        token=self.api.post('/api/v1/login',data={'username':'reader','password':'a-long-test-password'}).text.strip()
        other={'Authorization':'Bearer '+token}
        self.assertEqual(self.api.get(url+'?revision=1',headers=other).status_code,404)
        self.assertNotIn(sid+'\t',self.api.get('/api/v1/saves?scope=mine',headers=other).text)
        for i in range(8):self.assertEqual(self.upload(name='Public '+str(i)).status_code,201)
        first=self.api.get('/api/v1/saves?scope=public',headers=other).text.splitlines()
        second=self.api.get('/api/v1/saves?scope=public&page=1',headers=other).text.splitlines()
        self.assertEqual(first[0],'MORE\t1');self.assertEqual(len(first),8)
        self.assertEqual(second[0],'MORE\t0');self.assertEqual(len(second),2)
        public_id=first[1].split('\t')[0]
        self.assertEqual(self.api.get('/api/v1/saves/'+public_id+'/download?revision=1',headers=other).status_code,200)
        for query in ('scope=bad','page=-1','page=bad'):
            self.assertEqual(self.api.get('/api/v1/saves?'+query,headers=other).status_code,400)
        self.assertEqual(self.upload(header_offset='1').status_code,400)

    def test_public_owner_search_filters_before_pagination(self):
        from urllib.parse import unquote
        self.upload(name='Private', private='1')
        for i in range(8):
            self.assertEqual(self.upload(name='Public ' + str(i)).status_code, 201)
        with sqlite3.connect(self.path) as db:
            db.execute("UPDATE saves SET game='Power Stone',updated=123")
        self.register('reader')
        token = self.api.post('/api/v1/login', data={
            'username': 'reader', 'password': 'a-long-test-password'}).text.strip()
        reader = {'Authorization': 'Bearer ' + token}
        self.assertEqual(self.api.post('/api/v1/saves', headers=reader, data={
            'name': 'Unrelated', 'filename': 'OTHER',
            'save': (io.BytesIO(vms()), 'test.vms', 'application/octet-stream')}).status_code, 201)

        def listing(page, **filters):
            response = self.api.get('/api/v1/saves', headers=reader, query_string={
                'scope': 'public', 'user': 'TeStEr', 'page': page, **filters})
            self.assertEqual(response.status_code, 200)
            lines = response.text.splitlines()
            return lines[0], [[unquote(c) for c in row.split('\t')] for row in lines[1:]]

        first_more, first = listing(0, game='stone')
        last_more, last = listing(1, game='STONE')
        self.assertEqual((first_more, last_more), ('MORE\t1', 'MORE\t0'))
        self.assertEqual((len(first), len(last)), (7, 1))
        self.assertEqual([row[3] for row in first + last], ['Public ' + str(i) for i in reversed(range(8))])
        self.assertTrue(all(row[5] == 'tester' for row in first + last))
        sid = first[0][0]
        self.assertEqual(self.api.get('/api/v1/saves/' + sid + '/download?revision=1', headers=reader).data, vms())
        for filters in ({'user': 'test'}, {'user': 'nobody'}, {'game': 'Missing'},
                        {'game': '%'}, {'game': '_'}):
            self.assertEqual(listing(0, **filters), ('MORE\t0', []))
        # Public mode excludes the caller's own private entries too.
        own = self.api.get('/api/v1/saves?scope=public&user=tester', headers=self.auth)
        self.assertNotIn('Private', own.text)
        for query in ({'user': ''}, {'user': 'te'}, {'user': 'tester%'},
                      {'user': 'a' * 25}, {'game': 'x' * 81}, {'game': '\n'},
                      {'scope': 'mine', 'user': 'tester'}):
            response = self.api.get('/api/v1/saves', headers=reader,
                                    query_string={'scope': 'public', **query})
            self.assertEqual(response.status_code, 400)
        self.assertEqual(self.app.test_client().get('/api/v1/saves?scope=public&user=tester').status_code, 401)

    def test_public_owner_at_account_capacity(self):
        self.upload()
        with sqlite3.connect(self.path) as db:
            for i in range(199):
                db.execute('INSERT INTO saves(user_id,name,filename,game,notes,private,data,sha256,created,updated) '
                           'SELECT user_id,?,filename,game,notes,private,data,sha256,created,updated FROM saves WHERE id=1',
                           ('Save ' + str(i),))
        ids = []
        for page in range(29):
            response = self.api.get('/api/v1/saves', headers=self.auth,
                                    query_string={'scope': 'public', 'user': 'tester', 'page': page})
            self.assertEqual(response.status_code, 200)
            lines = response.text.splitlines()
            self.assertEqual(lines[0], 'MORE\t' + ('1' if page < 28 else '0'))
            self.assertEqual(len(lines)-1, 7 if page < 28 else 4)
            ids.extend(row.split('\t')[0] for row in lines[1:])
        self.assertEqual(len(set(ids)), 200)
        self.assertEqual(self.api.get('/api/v1/saves?scope=public&user=tester&page=29', headers=self.auth).text, 'MORE\t0\n')

    def test_ci_client_downloads(self):
        base='https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dcvmu-client.'
        for extension in ('elf','cdi'):
            r=self.web.get('/downloads/dcvmu-client.'+extension)
            self.assertEqual(r.status_code,302)
            self.assertEqual(r.headers['Location'],base+extension)
        account=self.web.get('/account').text
        self.assertNotIn(base+'cdi',account)
        self.assertNotIn(base+'elf',account)
        self.assertNotIn('Enable broadband adapter emulation',account)
        self.assertIn('/getting-started',account)

    def test_getting_started_is_omitted_for_console_browsers(self):
        desktop=self.web.get('/',headers={'User-Agent':'Mozilla/5.0'}).text
        self.assertIn('id="getting-started"',desktop)
        self.assertIn('href="/getting-started"',desktop)
        self.assertNotIn('guide-steps',desktop)
        guide=self.web.get('/getting-started').text
        self.assertIn('<h1>Get started on your Dreamcast</h1>',guide)
        self.assertIn('dcvmu-client.cdi">Download Dreamcast CD Image</a>',guide)
        self.assertIn('guide-steps',guide)
        for agent in ('DreamcastBrowser/0.1 (KallistiOS)', 'Mozilla/3.0 (DreamKey)', 'DreamPassport/3.0'):
            console=self.web.get('/',headers={'User-Agent':agent}).text
            self.assertNotIn('getting-started',console)
            self.assertNotIn('guide-steps',console)
            self.assertIn('Browse saves',console)
            self.assertIn('Download Dreamcast CD Image',console)
            guide=self.web.get('/getting-started',headers={'User-Agent':agent}).text
            self.assertNotIn('guide-steps',guide)

    def test_embedded_metadata(self):
        from vmu_validation import header_metadata
        self.assertEqual(header_metadata(b''), {})
        self.assertFalse(any(header_metadata(bytes(128)).values()))
        for offset in (0, 1):
            raw=bytearray(vms(offset=offset))
            start=offset*512
            raw[start:start+16]=b'VMU label'.ljust(16,b' ')
            description='ソニック <script>'.encode('shift_jis')
            raw[start+16:start+48]=description.ljust(32,b' ')
            raw[start+48:start+64]=b'GAME_ID\0ignored!'
            raw[start+70:start+72]=b'\0\0'
            struct.pack_into('<H',raw,start+70,binascii.crc_hqx(raw[start:start+384],0))
            result=self.upload(name='Metadata '+str(offset),private=str(offset),
                               save=(io.BytesIO(raw),'save.vms','application/octet-stream'))
            self.assertEqual(result.status_code,201)
            sid=result.text.splitlines()[1]
            detail='/saves/'+sid
            for path in (detail,) if offset else ('/',detail):
                page=self.web.get(path).text
                self.assertIn('ソニック &lt;script&gt;',page)
                self.assertIn('GAME_ID</dd>',page)
                self.assertNotIn('ignored!',page)
                console=self.web.get(path,headers={'User-Agent':'DreamcastBrowser/0.1'}).text
                self.assertNotIn('<dl class="save-metadata"',console)
                self.assertNotIn('GAME_ID',console)
            if offset:
                self.assertEqual(self.app.test_client().get(detail).status_code,404)
                self.assertNotIn('Metadata 1',self.app.test_client().get('/').text)

    def test_auth_csrf(self):
        with sqlite3.connect(self.path) as db:
            self.assertTrue(db.execute('select password_hash from users').fetchone()[0].startswith('$argon2id$'))
        self.assertEqual(self.web.post('/logout').status_code,403)
        self.assertEqual(self.api.post('/api/v1/saves').status_code,401)
        self.assertEqual(self.web.post('/api/v1/saves').status_code,401)
        self.assertEqual(self.api.get('/account',headers=self.auth).status_code,401)
        self.assertEqual(self.api.post('/api/v1/login',data={'username':'tester','password':'wrong'}).status_code,401)
        self.assertEqual(self.web.post('/logout',data={'csrf':self.csrf('/account')}).status_code,303)
        self.assertEqual(self.web.get('/account').status_code,401)
    def test_visibility_owner_and_download(self):
        r=self.upload(private='1');self.assertEqual(r.status_code,201);sid=r.text.splitlines()[1]
        guest=self.app.test_client()
        for suffix in ('','/download'):self.assertEqual(guest.get('/saves/'+sid+suffix).status_code,404)
        self.assertNotIn('My save',guest.get('/').text)
        self.assertEqual(self.web.get('/saves/'+sid+'/download').data,vms())
        self.assertIn('&lt;script&gt;',self.web.get('/saves/'+sid).text)
        csrf=self.csrf('/saves/'+sid)
        self.assertEqual(self.web.post('/saves/'+sid+'/edit',data={'csrf':csrf,'revision':'1','game':'New game','notes':'shared'}).status_code,303)
        self.assertIn('My save',guest.get('/?game=TEST_SAVE&user=tester').text)
        self.assertEqual(guest.get('/saves/'+sid+'/download').status_code,200)
        self.assertEqual(self.web.post('/saves/'+sid+'/edit',data={'csrf':csrf,'revision':'1','game':'stale'}).status_code,409)
        self.register('otheruser')
        self.assertEqual(self.web.post('/saves/'+sid+'/edit',data={'csrf':self.csrf('/account'),'revision':'2','game':'stolen'}).status_code,404)
    def test_duplicates(self):
        first=self.upload();self.assertEqual(first.status_code,201)
        self.assertEqual(self.upload().text,'CONFLICT\n1\n')
        self.assertEqual(self.upload(mode='replace',revision='0').status_code,409)
        r=self.upload(mode='replace',revision='1',save=(io.BytesIO(vms(b'y'*600)),'test.vms','application/octet-stream'))
        self.assertEqual(r.status_code,201);self.assertEqual(r.text.splitlines()[1],first.text.splitlines()[1])
        self.assertEqual(self.upload(mode='replace',revision='1').status_code,409)
        r=self.upload(mode='keep');self.assertEqual(r.status_code,201);self.assertIn('My save (2)',r.text)
        with sqlite3.connect(self.path) as db:self.assertEqual(db.execute('select count(*) from saves').fetchone()[0],2)
    def test_validation_and_logout(self):
        self.assertEqual(self.upload(filename='../bad').status_code,400)
        self.assertEqual(self.upload(save=(io.BytesIO(b'bad'),'test.vms','application/octet-stream')).status_code,400)
        self.assertEqual(self.upload(save=(io.BytesIO(b'x'*131584),'test.vms','application/octet-stream')).status_code,400)
        self.assertEqual(self.upload(private='no').status_code,400)
        self.assertEqual(self.api.post('/api/v1/logout',headers=self.auth).status_code,200)
        self.assertEqual(self.upload().status_code,401)
        self.assertEqual(self.web.post('/login',data={'csrf':self.csrf('/login'),'username':'tester','password':'a-long-test-password'},headers={'Origin':'https://evil.example'}).status_code,403)
    def test_concurrent_duplicate_decisions(self):
        self.assertEqual(self.upload().status_code,201)
        def replace(_):
            with self.app.test_client() as api:
                return api.post('/api/v1/saves',headers=self.auth,data={
                    'name':'My save','filename':'TEST_SAVE','game':'Test game',
                    'mode':'replace','revision':'1','save':(io.BytesIO(vms(b'z'*256)),'test.vms','application/octet-stream')}).status_code
        with ThreadPoolExecutor(max_workers=2) as pool:
            self.assertEqual(sorted(pool.map(replace,range(2))),[201,409])
    def test_rate_limit_and_expiry(self):
        with sqlite3.connect(self.path) as db:
            db.execute("UPDATE sessions SET expires=0 WHERE kind='api'")
        self.assertEqual(self.upload().status_code,401)
        with sqlite3.connect(self.path) as db:
            db.execute("UPDATE limits SET count=15, expires=9999999999 WHERE key='login-user:tester'")
        self.assertEqual(self.api.post('/api/v1/login',data={'username':'tester','password':'a-long-test-password'}).status_code,429)
    def test_upload_file_sanity(self):
        for raw in (b'x'*512, b'PK'+bytes(510), vms()+bytes(512), vms(b'x'*123265)):
            self.assertEqual(self.upload(save=(io.BytesIO(raw),'test.vms','application/octet-stream')).status_code,400)
        for pos in (64,68,72,140):
            raw=bytearray(vms());raw[pos]^=255
            self.assertEqual(self.upload(save=(io.BytesIO(raw),'test.vms','application/octet-stream')).status_code,400)
        for name,mime in [('save.zip','application/octet-stream'),('save.vms','text/html')]:
            self.assertEqual(self.upload(save=(io.BytesIO(vms()),name,mime)).status_code,400)
        self.assertEqual(self.api.post('/api/v1/saves',data=b'x'*196609,
                                      content_type='application/octet-stream',headers=self.auth).status_code,413)
        self.assertEqual(self.upload(save=[(io.BytesIO(vms()),'a.vms','application/octet-stream'),(io.BytesIO(vms()),'b.vms','application/octet-stream')]).status_code,400)
        for index,raw in enumerate((vms(b'x'*123264),vms(b'x'*122752),vms(b'x'*102272),vms(b'x'*98304),
                                   vms(icons=3),vms(eye=1),vms(eye=2),vms(eye=3),vms(offset=1))):
            self.assertEqual(self.upload(name='Valid '+str(index),save=(io.BytesIO(raw),'test.vms','application/octet-stream')).status_code,201)

    def test_cross_account_authorization(self):
        private_id=self.upload(name='Owner private',private='1').text.splitlines()[1]
        public_id=self.upload(name='Owner public').text.splitlines()[1]
        owner_web=self.web;owner_auth=self.auth
        with sqlite3.connect(self.path) as db:
            before=db.execute('SELECT * FROM saves ORDER BY id').fetchall()
            accounts=db.execute('SELECT * FROM users').fetchall()
        self.web=self.app.test_client();self.register('attacker')
        attacker_api=self.app.test_client()
        token=attacker_api.post('/api/v1/login',data={'username':'attacker','password':'a-long-test-password'}).text.strip()
        auth={'Authorization':'Bearer '+token}
        for sid in (private_id,public_id):
            r=self.web.post('/saves/'+sid+'/edit',data={'csrf':self.csrf('/account'),'revision':'1',
                       'game':'hijacked','private':'0','user_id':'1','username':'tester'})
            self.assertEqual(r.status_code,404)
        for client in (self.web,self.app.test_client()):
            for suffix in ('','/download'):
                self.assertEqual(client.get('/saves/'+private_id+suffix).status_code,404)
        self.assertEqual(attacker_api.get('/api/v1/saves/'+private_id+'/download?revision=1',headers=auth).status_code,404)
        self.assertEqual(attacker_api.get('/api/v1/saves/'+public_id+'/download?revision=1',headers=auth).data,vms())
        self.assertEqual(attacker_api.get('/api/v1/saves/'+public_id+'/download?revision=0',headers=auth).status_code,409)
        for scope in ('mine','public'):
            listing=attacker_api.get('/api/v1/saves?scope='+scope+'&user_id=1',headers=auth).text
            self.assertNotIn('Owner%20private',listing)
            if scope=='mine':self.assertNotIn('Owner%20public',listing)
        self.assertNotIn('Owner private',self.web.get('/account?user_id=1').text)
        self.assertNotIn('Owner private',self.web.get('/?user=tester').text)
        self.assertNotIn('tester@example.com',self.web.get('/saves/'+public_id).text)
        self.api=attacker_api;self.auth=auth
        self.assertEqual(self.upload(name='Owner public',mode='replace',revision='1',user_id='1',id=public_id).status_code,409)
        self.assertEqual(self.upload(name='Owner public',user_id='1',id=public_id).status_code,201)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT * FROM saves WHERE user_id=1 ORDER BY id').fetchall(),before)
            self.assertEqual(db.execute('SELECT * FROM users WHERE id=1').fetchall(),accounts)
            self.assertEqual(db.execute('SELECT user_id FROM saves ORDER BY id DESC LIMIT 1').fetchone()[0],2)
        # Supplying somebody else's token in the body cannot change logout's target.
        attacker_api.post('/api/v1/logout',headers=auth,data={'token':owner_auth['Authorization'][7:],'user_id':'1'})
        self.assertEqual(attacker_api.get('/api/v1/me',headers=auth).status_code,401)
        self.assertEqual(attacker_api.get('/api/v1/me',headers=owner_auth).text,'tester\n')
        self.assertEqual(owner_web.get('/account').status_code,200)

    def test_csrf_rejects_malformed_and_other_sessions(self):
        other=self.app.test_client()
        csrf=re.search(r'name="csrf" value="([^"]+)"',other.get('/login').text)[1]
        for value in ('',csrf,'é'*43):
            self.assertEqual(self.web.post('/logout',data={'csrf':value}).status_code,403)
        self.assertEqual(self.web.get('/account').status_code,200)

    def test_visibility_edit_rejects_invalid_value(self):
        sid=self.upload(private='1').text.splitlines()[1]
        r=self.web.post('/saves/'+sid+'/edit',data={'csrf':self.csrf('/account'),'revision':'1','game':'test','private':'garbage'})
        self.assertEqual(r.status_code,400)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT private,revision FROM saves').fetchone(),(1,1))

    def test_token_kinds_and_session_rotation(self):
        web_cookie=self.web.get_cookie('dcvmu_session').value
        self.assertEqual(self.api.get('/api/v1/me',headers={'Authorization':'Bearer '+web_cookie}).status_code,401)
        forged=self.app.test_client();forged.set_cookie('dcvmu_session',self.auth['Authorization'][7:])
        self.assertEqual(forged.get('/account').status_code,401)
        csrf=self.csrf('/login')
        self.assertEqual(self.web.post('/login',data={'csrf':csrf,'username':'tester','password':'a-long-test-password'}).status_code,303)
        forged.set_cookie('dcvmu_session',web_cookie)
        self.assertEqual(forged.get('/account').status_code,401)
        self.assertNotEqual(self.web.get_cookie('dcvmu_session').value,web_cookie)

    def test_header_offset_matches_content(self):
        self.assertEqual(self.upload(save=(io.BytesIO(vms(icons=3)),'save.vms','application/octet-stream'),header_offset='1').status_code,400)
        self.assertEqual(self.upload(save=(io.BytesIO(vms(offset=1)),'save.vms','application/octet-stream'),header_offset='0').status_code,400)
        r=self.upload(save=(io.BytesIO(vms(offset=1)),'save.vms','application/octet-stream'),header_offset='1')
        self.assertEqual(r.status_code,201)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT header_offset FROM saves').fetchone()[0],1)

    def test_upload_timestamps_survive_metadata_edits(self):
        from unittest.mock import patch
        with patch('app.time.time',return_value=1700000000):
            result=self.upload()
        self.assertEqual(result.status_code,201)
        sid=result.text.splitlines()[1]
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT created,uploaded_at FROM saves WHERE id=?',(sid,)).fetchone(),(1700000000,1700000000))
        csrf=self.csrf('/saves/'+sid)
        result=self.web.post('/saves/'+sid+'/edit',data={'csrf':csrf,'revision':'1','game':'Changed title','notes':'Changed notes'})
        self.assertEqual(result.status_code,303)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT uploaded_at FROM saves WHERE id=?',(sid,)).fetchone()[0],1700000000)
        with patch('app.time.time',return_value=1700000100):
            self.assertEqual(self.upload(mode='replace',revision='2').status_code,201)
        for path in ('/','/account','/saves/'+sid):
            page=self.web.get(path).text
            self.assertIn('2023-11-14T22:15:00Z',page)
            self.assertIn('/static/local-time.js',page)
            console=self.web.get(path,headers={'User-Agent':'DreamcastBrowser/0.1'}).text
            self.assertIn('2023-11-14 22:15:00 UTC',console)
            self.assertNotIn('/static/local-time.js',console)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT created,uploaded_at FROM saves WHERE id=?',(sid,)).fetchone(),(1700000000,1700000100))

    def test_legacy_upload_timestamp_migration(self):
        self.assertEqual(self.upload().status_code,201)
        self.assertEqual(self.upload(name='Old edited save').status_code,201)
        with sqlite3.connect(self.path) as db:
            db.execute("UPDATE saves SET revision=2 WHERE name='Old edited save'")
            db.execute('ALTER TABLE saves DROP COLUMN uploaded_at')
        create_app({'TESTING':True,'DATABASE':self.path})
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute("SELECT uploaded_at=created FROM saves WHERE name='My save'").fetchone()[0],1)
            self.assertIsNone(db.execute("SELECT uploaded_at FROM saves WHERE name='Old edited save'").fetchone()[0])
        self.assertIn('<dt>Uploaded</dt>',self.web.get('/').text)
        self.assertNotIn('First uploaded',self.web.get('/').text)

    def test_headers_https(self):
        r=self.web.get('/');self.assertIn("script-src 'self'",r.headers['Content-Security-Policy'])
        self.assertEqual(r.headers['Cache-Control'],'no-store')
        r=self.app.test_client().get('/register')
        for attr in ('Secure','HttpOnly','SameSite=Lax'):self.assertIn(attr,r.headers['Set-Cookie'])
        self.app.testing=False
        self.assertEqual(self.web.get('/').status_code,400)
        self.assertEqual(self.web.get('/',headers={'X-Forwarded-Proto':'https'}).status_code,200)

if __name__=='__main__':unittest.main()
