"""Host checks that the launcher uses Flycast's own VMUs and backs them up."""
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
        (self.images/'game_vmu_save_A1.bin').write_bytes(card('PER_GAME'))
        (self.images/'vmu_save_A1.bin').write_bytes(card('CHU_CHU__RCT','DCVMU_AUTH'))
        (self.images/'vmu_save_A2.bin').write_bytes(card())
        self.backups=self.home/'Library/Application Support/DCVMU/backups'
        self.env=dict(os.environ,HOME=str(self.home))
    def tearDown(self):self.temp.cleanup()
    def run_launcher(self,*args):
        return subprocess.run([sys.executable,str(SCRIPT),'--elf','/tmp/test.elf','--flycast','/bin/false',*args],env=self.env,text=True,capture_output=True)
    def test_launches_on_flycast_cards_with_keyboard_on_port_d(self):
        fake=self.home/'flycast'
        fake.write_text('#!'+sys.executable+'\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n')
        fake.chmod(0o700)
        before={p:p.read_bytes() for p in self.images.iterdir()}
        result=self.run_launcher('--flycast',str(fake))
        self.assertEqual(result.returncode,0,result.stderr)
        config=dict(item.split('=',1) for item in json.loads(result.stdout.splitlines()[-1])[1].split(','))
        keyboard_port=int(config['input:maple_sdl_keyboard'])
        self.assertEqual(keyboard_port,3)
        self.assertEqual(config[f'input:device{keyboard_port+1}'],'5')
        self.assertEqual(config['config:PerGameVmu'],'no')
        # Cards and controller ports stay exactly as Flycast is configured.
        self.assertFalse({'config:Dreamcast.VMUPath','input:device1','input:device1.1'}&set(config))
        self.assertTrue(all(p.read_bytes()==data for p,data in before.items()))
    def test_lists_shared_cards_only(self):
        result=self.run_launcher('--list-vmus')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout.splitlines(),['A1: vmu_save_A1.bin (1 saves)','  CHU_CHU__RCT','A2: vmu_save_A2.bin (0 saves)'])
        self.assertFalse(self.backups.exists())
    def test_each_card_version_is_backed_up_once(self):
        self.assertEqual(self.run_launcher('--dry-run').returncode,0)
        self.assertEqual(self.run_launcher('--dry-run').returncode,0)
        self.assertEqual(sorted(p.name.split('-')[0] for p in self.backups.glob('*.bin')),['vmu_save_A1','vmu_save_A2'])
        (self.images/'vmu_save_A1.bin').write_bytes(card('NEW_SAVE'))
        self.assertEqual(self.run_launcher('--dry-run').returncode,0)
        self.assertEqual(len(list(self.backups.glob('vmu_save_A1-*.bin'))),2)
        self.assertIn(card('CHU_CHU__RCT','DCVMU_AUTH'),[p.read_bytes() for p in self.backups.glob('*.bin')])

if __name__=='__main__':unittest.main()
