"""Host checks for VMU discovery, live cards and non-destructive persistent copies."""
import os
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'flycast-vmus.py'

def card(*names):
    data=bytearray(131072)
    struct.pack_into('<HH',data,255*512+0x4a,253,13)
    for i,name in enumerate(names):
        data[253*512+i*32]=0x33
        data[253*512+i*32+4:253*512+i*32+16]=name.encode().ljust(12,b'\0')
    return bytes(data)

class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.home=Path(self.temp.name)
        self.images=self.home/'Library/Application Support/Flycast/data'
        self.images.mkdir(parents=True)
        for i in range(6):
            (self.images/f'game{i}_vmu_save_A1.bin').write_bytes(bytes([i])*131072)
        (self.images/'vmu_save_A1.bin').write_bytes(card('CHU_CHU__RCT'))
        for slot in ('A2','B1'):
            (self.images/f'vmu_save_{slot}.bin').write_bytes(card())
        self.env=dict(os.environ,HOME=str(self.home))
    def tearDown(self):self.temp.cleanup()
    def run_launcher(self,*args):
        return subprocess.run([sys.executable,str(SCRIPT),'--elf','/tmp/test.elf','--flycast','/bin/false',*args],env=self.env,text=True,capture_output=True)
    def test_host_keyboard_is_routed_to_keyboard_port(self):
        fake=self.home/'flycast'
        fake.write_text('#!'+sys.executable+'\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n')
        fake.chmod(0o700)
        result=self.run_launcher('--flycast',str(fake),'--copies')
        self.assertEqual(result.returncode,0,result.stderr)
        command=json.loads(result.stdout.splitlines()[-1])
        config=dict(item.split('=',1) for item in command[1].split(','))
        keyboard_port=int(config['input:maple_sdl_keyboard'])
        self.assertEqual(keyboard_port,3)
        self.assertEqual(config[f'input:device{keyboard_port+1}'],'5')
        for port in (1,2,3):
            self.assertEqual(config[f'input:device{port}'],'0')

    def test_default_uses_flycast_cards_in_place(self):
        fake=self.home/'flycast'
        fake.write_text('#!'+sys.executable+'\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n')
        fake.chmod(0o700)
        before={p:p.read_bytes() for p in self.images.iterdir()}
        result=self.run_launcher('--flycast',str(fake))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('A1: vmu_save_A1.bin (1 saves)',result.stdout)
        self.assertNotIn('game',result.stdout)
        config=dict(item.split('=',1) for item in json.loads(result.stdout.splitlines()[-1])[1].split(','))
        self.assertEqual(config['config:PerGameVmu'],'no')
        self.assertEqual((config['input:device4'],config['input:maple_sdl_keyboard']),('5','3'))
        self.assertFalse({'config:Dreamcast.VMUPath','input:device1','input:device1.1'}&set(config))
        self.assertTrue(all(p.read_bytes()==data for p,data in before.items()))
        root=self.home/'Library/Application Support/DCVMU'
        self.assertEqual(sorted(p.name.split('-')[0] for p in (root/'backups').glob('*.bin')),['vmu_save_A1','vmu_save_A2','vmu_save_B1'])
        self.assertEqual(list((root/'VMUs').glob('*.bin')),[])
        self.assertNotEqual(self.run_launcher('--bank','2','--dry-run').returncode,0)
    def test_copies_mount_shared_cards_with_one_blank(self):
        result=self.run_launcher('--copies','--list-vmus')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('2 images, 1 banks',result.stdout)
        self.assertIn('Bank 1, A2: vmu_save_A1.bin (1 saves)',result.stdout)
        self.assertIn('Bank 1, B1: vmu_save_A2.bin (0 saves)',result.stdout)
        self.assertNotIn('game',result.stdout)
        self.assertNotEqual(self.run_launcher('--copies','--bank','2','--dry-run').returncode,0)
    def test_banks_cover_explicit_directory(self):
        result=self.run_launcher('--vmus-dir',str(self.images),'--list-vmus')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('9 images, 2 banks',result.stdout)
        self.assertIn('Bank 2, A2: game4',result.stdout)
        self.assertNotEqual(self.run_launcher('--vmus-dir',str(self.images),'--bank','3','--dry-run').returncode,0)
    def test_test_writes_survive_without_changing_originals(self):
        before={p:p.read_bytes() for p in self.images.iterdir()}
        self.assertEqual(self.run_launcher('--copies','--dry-run').returncode,0)
        copies=list((self.home/'Library/Application Support/DCVMU/VMUs').glob('*.bin'))
        self.assertEqual(len(copies),2)
        changed=copies[0];changed.write_bytes(b't'*131072)
        self.assertEqual(self.run_launcher('--copies','--dry-run').returncode,0)
        self.assertEqual(changed.read_bytes(),b't'*131072)
        self.assertTrue(all(p.read_bytes()==data for p,data in before.items()))
        self.assertEqual(self.run_launcher('--vmus-dir',str(self.images),'--bank','2','--dry-run').returncode,0)
        self.assertEqual(len(list(changed.parent.glob('*.bin'))),5)

if __name__=='__main__':unittest.main()
