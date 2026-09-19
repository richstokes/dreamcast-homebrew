import base64
import hashlib
import io
import json
import sqlite3
import struct
import time
import unittest

import test_service as fixtures
from vmu_tools import (ICON_NAME, UNLOCK, build_icondata, validate_icondata, icon_header,
                       icon_png, extract_image, swap_words)


def icon_data(unlock=False):
    return build_icondata('MY VMU', '10'*512, '01'*512, 'fffff123'+'f000'*14, unlock)


def directory_entry(name, data, filetype=0x33, offset=0, start=0):
    entry = bytearray(32)
    entry[0] = filetype
    struct.pack_into('<H', entry, 2, start)
    entry[4:16] = name.encode('ascii').ljust(12, b'\0')
    struct.pack_into('<HH', entry, 24, len(data)//512, offset)
    return bytes(entry)


def card_image(files):
    """Synthetic standard card, deliberately using non-contiguous save chains."""
    card = bytearray(128*1024)
    root = 255*512
    card[root:root+16] = b'\x55'*16
    struct.pack_into('<9H', card, root+0x40, 255, 0, 255, 254, 1, 253, 13, 0, 200)
    fat = [0xFFFC]*256
    fat[255] = fat[254] = fat[241] = 0xFFFA
    for block in range(242,254): fat[block] = block-1
    available = list(range(0,200,2)) + list(range(1,200,2))
    for index, (name, data, filetype, offset) in enumerate(files):
        blocks = [available.pop(0) for _ in range(len(data)//512)]
        for i, block in enumerate(blocks):
            card[block*512:(block+1)*512] = data[i*512:(i+1)*512]
            fat[block] = blocks[i+1] if i+1<len(blocks) else 0xFFFA
        pos = (253-index//16)*512 + (index%16)*32
        card[pos:pos+32] = directory_entry(name,data,filetype,offset,blocks[0])
    struct.pack_into('<256H',card,254*512,*fat)
    return bytes(card)


class FormatTest(unittest.TestCase):
    def test_icon_layout_preview_and_unlock(self):
        data=icon_data(True)
        self.assertEqual(len(data),1024)
        self.assertEqual(validate_icondata(data),(24,152))
        self.assertEqual(data[24:152],b'\xaa'*128)
        self.assertEqual(data[184:696],b'\x01'*512)
        self.assertEqual(data[704:720],UNLOCK)
        self.assertEqual(icon_header(data)[96:128],data[152:184])
        self.assertEqual(icon_header(data,True)[128:],b'\x10'*512)
        self.assertTrue(icon_png(data).startswith(b'\x89PNG'))
        self.assertEqual(icon_data(False)[704:720],bytes(16))

    def test_icon_invalid_inputs_and_offsets(self):
        valid=['MY VMU','0'*1024,'1'*1024,'ffff'*16]
        for i,value in ((0,'bad\nlabel'),(0,'é'),(0,'x'*17),(1,'0'*1023),(1,'x'*1024),(2,'g'*1024),(3,'ff'*16)):
            args=valid[:];args[i]=value
            with self.assertRaises(ValueError):build_icondata(*args)
        for mono,color in ((0,0),(1,152),(24,25),(24,700),(0xFFFFFFFF,152)):
            data=bytearray(icon_data());struct.pack_into('<II',data,16,mono,color)
            with self.assertRaises(ValueError):validate_icondata(data)
        for data in (b'',b'x'*24,icon_data()[:-1]):
            with self.assertRaises(ValueError):validate_icondata(data)

    def test_all_import_formats_preserve_bytes_and_header_offset(self):
        raw=fixtures.vms(payload=b'hello'*200,icons=1,offset=1)
        card=card_image([('TEST_SAVE',raw,0x33,1)])
        for filename,contents in [('a.bin',card),('A.VMU',card),('a.dcm',swap_words(card)),
                ('a.dci',directory_entry('TEST_SAVE',raw,offset=1)+swap_words(raw))]:
            with self.subTest(filename=filename):
                rows=extract_image(contents,filename)
                self.assertEqual(len(rows),1)
                self.assertEqual(rows[0]['reason'],'')
                self.assertEqual(rows[0]['header_offset'],1)
                self.assertEqual(base64.b64decode(rows[0]['data']),raw)

    def test_excluded_payloads_never_reach_staging(self):
        auth=fixtures.vms(payload=b'DCVMU-AUTH-V1 SECRET_TOKEN')
        renamed=bytearray(fixtures.vms(offset=1));renamed[512+48:512+64]=b'DCVMU_AUTH'.ljust(16,b'\0')
        broken=bytearray(fixtures.vms());broken[135]^=0xFF
        card=card_image([('DCVMU_AUTH',auth,0x33,0),('RENAMED',auth,0x33,0),
            ('APPID',bytes(renamed),0x33,1),('GAME',fixtures.vms(),0xCC,1),
            ('BAD',bytes(broken),0x33,0),('ICONDATA_VMS',icon_data(),0x33,0)])
        rows=extract_image(card,'test.bin')
        self.assertTrue(all('data' not in row for row in rows[:5]))
        self.assertEqual(rows[5]['kind'],'icon')
        self.assertNotIn('SECRET_TOKEN',json.dumps(rows))

    def test_directory_offsets_truncation_and_nexus_errors(self):
        raw=fixtures.vms(offset=1)
        bad=directory_entry('TEST',raw,offset=0)+swap_words(raw)
        self.assertNotIn('data',extract_image(bad,'bad.dci')[0])
        for data,name in [(b'','a.bin'),(bytes(128*1024),'a.vmu'),(b'x'*128*1024,'a.zip'),
                (directory_entry('TEST',raw)+swap_words(raw)[:-4],'a.dci')]:
            with self.assertRaises(ValueError):extract_image(data,name)
        card=bytearray(card_image([]))
        struct.pack_into('<H',card,255*512+0x46,253)
        with self.assertRaises(ValueError):extract_image(card,'a.bin')

    def test_corrupt_chains_and_cross_links(self):
        raw=fixtures.vms(payload=b'x'*1000)
        initial=card_image([('FIRST',raw,0x33,0),('SECOND',raw,0x33,0)])
        for link in (0,255,0xFFFC,0xFFFF,0xFFFA):
            card=bytearray(initial);struct.pack_into('<H',card,254*512,link)
            self.assertNotIn('data',extract_image(card,'a.bin')[0])
        card=bytearray(initial)
        struct.pack_into('<H',card,253*512+32+2,0)
        rows=extract_image(card,'a.bin')
        self.assertTrue(all('data' not in row for row in rows))
        self.assertTrue(all('shared' in row['reason'] for row in rows))


class ToolsServiceTest(unittest.TestCase):
    setUp=fixtures.ServiceTest.setUp
    tearDown=fixtures.ServiceTest.tearDown
    csrf=fixtures.ServiceTest.csrf
    register=fixtures.ServiceTest.register

    def studio(self,**overrides):
        data=dict(csrf=self.csrf('/studio'),name='My icons',label='MY VMU',
                  mono='10'*512,pixels='01'*512,palette='fffff123'+'f000'*14,private='1',unlock='1')
        data.update(overrides)
        return self.web.post('/studio',data=data)

    def stage(self,raw=None):
        if raw is None:raw=card_image([('TEST_SAVE',fixtures.vms(),0x33,0),('ICONDATA_VMS',icon_data(),0x33,0)])
        response=self.web.post('/import',data=dict(csrf=self.csrf('/import'),image=(io.BytesIO(raw),'test.bin')))
        self.assertEqual(response.status_code,303)
        return response.location

    def test_studio_private_previews_and_legacy_client_gate(self):
        r=self.studio();self.assertEqual(r.status_code,303)
        sid=int(r.location.split('/')[-1])
        detail=self.web.get(r.location)
        self.assertIn('Hidden 3D BIOS animation enabled',detail.text)
        self.assertIn('screen=vmu',detail.text)
        self.assertEqual(self.web.get(r.location+'/icon.png?screen=vmu').status_code,200)
        self.assertEqual(self.app.test_client().get(r.location).status_code,404)
        self.assertEqual(self.app.test_client().get(r.location+'/icon.png').status_code,404)
        self.assertNotIn('ICONDATA',self.api.get('/api/v1/saves',headers=self.auth).text)
        response=self.api.get('/api/v1/saves?include_icons=1',headers=self.auth)
        self.assertIn('ICONDATA_VMS',response.text)
        base=f'/api/v1/saves/{sid}'
        self.assertEqual(self.api.get(base+'/download?revision=1',headers=self.auth).status_code,400)
        download=self.api.get(base+'/download?revision=1&include_icons=1',headers=self.auth)
        self.assertEqual(download.data,icon_data(True))
        header=self.api.get(base+'/icon-header?revision=1',headers=self.auth)
        self.assertEqual(header.data,icon_header(icon_data(True)))
        self.assertEqual(self.api.get(base+'/icon-header?revision=2',headers=self.auth).status_code,409)
        self.assertEqual(self.api.get(base+'/icon-header?revision=1').status_code,401)
        self.assertEqual(self.web.get(r.location+'/download').data,icon_data(True))

    def test_studio_public_and_untrusted_pixels(self):
        r=self.studio(private='0',unlock='0')
        self.assertEqual(self.app.test_client().get(r.location).status_code,200)
        self.assertIn('My icons',self.app.test_client().get('/').text)
        self.assertEqual(self.studio(pixels='z'*1024).status_code,400)
        self.assertEqual(self.studio(csrf='').status_code,403)
        self.assertEqual(self.app.test_client().get('/studio').status_code,401)

    def test_import_selected_private_byte_exact_and_consumed(self):
        path=self.stage()
        page=self.web.get(path)
        self.assertIn('2 supported files',page.text)
        with sqlite3.connect(self.path) as db:self.assertEqual(db.execute('SELECT count(*) FROM saves').fetchone()[0],0)
        response=self.web.post(path,data={'csrf':self.csrf(path),'entry':['0'],'private':'1'})
        self.assertEqual(response.status_code,303)
        with sqlite3.connect(self.path) as db:
            rows=db.execute('SELECT filename,data,private FROM saves').fetchall()
            self.assertEqual(rows,[('TEST_SAVE',fixtures.vms(),1)])
            self.assertEqual(db.execute('SELECT count(*) FROM imports').fetchone()[0],0)
        self.assertEqual(self.web.get(path).status_code,404)
        self.assertEqual(self.app.test_client().get('/').text.count('Test save'),0)

    def test_import_duplicates_keep_both_and_icons_reach_new_client(self):
        for _ in range(2):
            path=self.stage()
            r=self.web.post(path,data={'csrf':self.csrf(path),'entry':['0','1'],'private':'0'})
            self.assertEqual(r.status_code,303)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM saves').fetchone()[0],4)
            self.assertEqual(len({r[0] for r in db.execute('SELECT name FROM saves')}),4)
        self.assertIn('ICONDATA_VMS',self.api.get('/api/v1/saves?include_icons=1',headers=self.auth).text)
        self.assertNotIn('ICONDATA_VMS',self.api.get('/api/v1/saves',headers=self.auth).text)

    def test_import_credentials_are_not_stored_even_before_confirmation(self):
        raw=fixtures.vms(payload=b'DCVMU-AUTH-V1 secret-token-do-not-stage')
        path=self.stage(card_image([('DCVMU_AUTH',raw,0x33,0)]))
        self.assertIn('Login credentials',self.web.get(path).text)
        with sqlite3.connect(self.path) as db:
            contents=db.execute('SELECT contents FROM imports').fetchone()[0]
            self.assertNotIn('data',json.loads(contents)[0])
            self.assertNotIn(base64.b64encode(raw).decode(),contents)
        self.assertEqual(self.web.post(path,data={'csrf':self.csrf(path),'entry':'0'}).status_code,400)

    def test_import_owner_expiry_cancel_and_selection_validation(self):
        path=self.stage()
        other=self.app.test_client()
        self.assertEqual(other.get(path).status_code,401)
        original=self.web;self.web=other;self.register('other');self.web=original
        self.assertEqual(other.get(path).status_code,404)
        self.assertEqual(other.get(path+'/0/icon.png').status_code,404)
        for entries in ([],['-1'],['2'],['0','0'],['0','00'],['not-a-number']):
            self.assertEqual(self.web.post(path,data={'csrf':self.csrf(path),'entry':entries}).status_code,400)
        self.assertEqual(self.web.post(path,data={'csrf':'','entry':['0']}).status_code,403)
        with sqlite3.connect(self.path) as db:db.execute('UPDATE imports SET expires=0')
        self.assertEqual(self.web.get(path).status_code,404)
        path=self.stage()
        response=self.web.post(path+'/cancel',data={'csrf':self.csrf(path)})
        self.assertEqual(response.status_code,303)
        self.assertEqual(self.web.get(path).status_code,404)

    def test_import_capacity_failure_is_atomic(self):
        with sqlite3.connect(self.path) as db:
            uid=db.execute('SELECT id FROM users').fetchone()[0]
            for i in range(199):
                db.execute('INSERT INTO saves(user_id,name,filename,game,notes,private,data,sha256,created,updated) VALUES (?,?,?,?,?,?,?,?,0,0)',
                           (uid,f'Save {i}','TEST','Test','',1,fixtures.vms(),'fixture'))
        path=self.stage()
        r=self.web.post(path,data={'csrf':self.csrf(path),'entry':['0','1'],'private':'1'})
        self.assertEqual(r.status_code,400)
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM saves').fetchone()[0],199)
            self.assertEqual(db.execute('SELECT count(*) FROM imports').fetchone()[0],1)
        self.assertEqual(self.web.get(path).status_code,200)

    def test_import_file_limits_and_auth(self):
        token=self.csrf('/import')
        self.assertEqual(self.web.post('/import',data={'csrf':token}).status_code,400)
        for name,raw in [('bad.zip',b'zip'),('card.bin',bytes(128*1024+1)),('huge.bin',bytes(200*1024))]:
            response=self.web.post('/import',data={'csrf':token,'image':(io.BytesIO(raw),name)})
            self.assertIn(response.status_code,(400,413))
        self.assertEqual(self.app.test_client().get('/import').status_code,401)


if __name__=='__main__':unittest.main()
