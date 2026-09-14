"""Host checks for bank discovery and non-destructive persistent VMU copies."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'flycast-vmus.py'

class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.home=Path(self.temp.name)
        self.images=self.home/'Library/Application Support/Flycast/data'
        self.images.mkdir(parents=True)
        for i in range(6):
            (self.images/f'game{i}_vmu_save_A1.bin').write_bytes(bytes([i])*131072)
        self.env=dict(os.environ,HOME=str(self.home))
    def tearDown(self):self.temp.cleanup()
    def run_launcher(self,*args):
        return subprocess.run([sys.executable,str(SCRIPT),'--elf','/tmp/test.elf','--flycast','/bin/false',*args],env=self.env,text=True,capture_output=True)
    def test_banks_cover_all_images(self):
        result=self.run_launcher('--list-vmus')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('6 images, 2 banks',result.stdout)
        self.assertIn('Bank 2, A2: game5',result.stdout)
        self.assertNotEqual(self.run_launcher('--bank','3','--dry-run').returncode,0)
    def test_test_writes_survive_without_changing_originals(self):
        before={p:p.read_bytes() for p in self.images.iterdir()}
        self.assertEqual(self.run_launcher('--dry-run').returncode,0)
        copies=list((self.home/'Library/Application Support/DCVMU/VMUs').glob('*.bin'))
        self.assertEqual(len(copies),5)
        changed=copies[0];changed.write_bytes(b't'*131072)
        self.assertEqual(self.run_launcher('--dry-run').returncode,0)
        self.assertEqual(changed.read_bytes(),b't'*131072)
        self.assertTrue(all(p.read_bytes()==data for p,data in before.items()))
        self.assertEqual(self.run_launcher('--bank','2','--dry-run').returncode,0)
        self.assertEqual(len(list(changed.parent.glob('*.bin'))),6)

if __name__=='__main__':unittest.main()
