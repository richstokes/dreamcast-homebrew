"""Host checks that Flycast runners with a custom VMU folder cannot move real cards."""
from pathlib import Path
import tempfile
import unittest

from isolated_cards import SLOTS, blank_cards

REPO = Path(__file__).resolve().parents[4]

class IsolatedCardsTest(unittest.TestCase):
    def test_every_slot_gets_a_blank_card(self):
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory)/'vmus'
            blank_cards(folder)
            self.assertEqual(sorted(p.name for p in folder.iterdir()),[f'vmu_save_{slot}.bin' for slot in SLOTS])
            self.assertEqual(len(SLOTS),8)
            for p in folder.iterdir():self.assertEqual(p.read_bytes(),bytes(131072))

    def test_scripts_with_a_custom_vmu_folder_fill_it_first(self):
        # An empty Dreamcast.VMUPath folder makes Flycast move the real cards into it.
        scripts=[p for p in REPO.glob('projects/**/*.py') if '.venv' not in p.parts and p!=Path(__file__).resolve()
                 and 'config:Dreamcast.VMUPath=' in p.read_text(errors='replace')]
        self.assertGreaterEqual(len(scripts),4)
        for script in scripts:
            source=script.read_text()
            with self.subTest(script=script.name):
                self.assertTrue('blank_cards(' in source or
                                "vmu_save_{slot}.bin').write_bytes(bytes(131072))" in source)

if __name__=='__main__':
    unittest.main()
