import io
import struct
import binascii
import re
import sqlite3
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from app import create_app

def vms(payload=b'x'*256, icons=0, eye=0, offset=0):
    eye_bytes=(0,8064,4544,2048)[eye]
    data=bytearray(128+icons*512+eye_bytes+len(payload))
    data[:16]=b'Test save       '
    struct.pack_into('<HHHHI',data,64,icons,1,eye,0,len(payload))
    data[-len(payload):]=payload
    struct.pack_into('<H',data,70,binascii.crc_hqx(data,0))
    data=bytes(offset*512)+bytes(data)
    return data+bytes((-len(data))%512)

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

    def test_ci_client_downloads(self):
        base='https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dcvmu-client.'
        for extension in ('elf','cdi'):
            r=self.web.get('/downloads/dcvmu-client.'+extension)
            self.assertEqual(r.status_code,302)
            self.assertEqual(r.headers['Location'],base+extension)
        account=self.web.get('/account').text
        self.assertIn(base+'cdi',account);self.assertIn(base+'elf',account)
        self.assertLess(account.index(base+'cdi'),account.index(base+'elf'))

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
        self.assertIn('My save',guest.get('/?game=New+game&user=tester').text)
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
