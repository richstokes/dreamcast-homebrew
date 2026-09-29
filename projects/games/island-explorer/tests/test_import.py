# /// script
# dependencies = ["numpy>=2,<3", "pillow>=11,<13"]
# ///
import sys,struct,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from disc import prs
from pvr import decode
from ninja import Ninja,transform

class ImportTests(unittest.TestCase):
    def test_literal_prs(self):
        self.assertEqual(prs(b'\x2fABCD\0\0'),b'ABCD')
    def test_overlapping_back_reference(self):
        self.assertEqual(prs(b'\x47ABC\xfd\x01\0\0'),b'ABCABC')
    def test_invalid_back_reference(self):
        with self.assertRaises(ValueError):prs(b'\x00\xff')
    def test_truncated_prs(self):
        with self.assertRaises(ValueError):prs(b'\x01')
    def test_output_bound(self):
        with self.assertRaises(ValueError):prs(b'\x2fABCD\0\0',limit=2)
    def test_twiddled_texture_order(self):
        image=decode(struct.pack('<4H',0xf800,0x07e0,0x001f,0xffff),1,1,2,2)
        self.assertEqual(image.getpixel((0,0)),(255,0,0,255))
        self.assertEqual(image.getpixel((0,1)),(0,255,0,255))
        self.assertEqual(image.getpixel((1,0)),(0,0,255,255))
        self.assertEqual(image.getpixel((1,1)),(255,255,255,255))
    def test_argb4444_alpha(self):
        image=decode(struct.pack('<4H',0x8fff,0,0,0),2,1,2,2)
        self.assertEqual(image.getpixel((0,0)),(255,255,255,136))
    def test_invalid_ninja_pointer(self):
        with self.assertRaises(ValueError):Ninja(bytes(16)).unpack('I',-1)
        with self.assertRaises(ValueError):Ninja(bytes(16)).unpack('I',14)
    def test_transform_translation(self):
        self.assertEqual((transform((10,20,30)) @ [1,2,3,1]).tolist(),[11,22,33,1])
    def test_ninja_alternate_rotation_order(self):
        point=transform(rotation=(16384,16384,16384),zyx=True) @ [1,2,3,1]
        for actual,expected in zip(point,[1,-3,2,1]):self.assertAlmostEqual(actual,expected)
if __name__=='__main__':unittest.main()
