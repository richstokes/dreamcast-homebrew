#!/usr/bin/env python3
"""Host tests for the serial profile analyzer; no SDK or emulator required."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
ANALYZER_PATH = Path(__file__).resolve().parents[1] / "scripts/analyze-profile.py"
SPEC = importlib.util.spec_from_file_location("analyze_profile", ANALYZER_PATH)
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


def record(iterations=100, frames=10, keys=6, wait=100, draw=2000, extra=""):
    return (f"PROF iters {iterations} frames {frames} keys {keys} | "
            f"wait {wait} us/it {extra}input 5 us/it draw {draw} us/frame "
            "(max 3000) present 10 us/frame | iter max 5000 us\n")


class ProfileTests(unittest.TestCase):
    def test_legacy_and_poll_records(self):
        profile = analyzer.ProfileSummary()
        profile.add_line("\x1b[32mserial: " + record())
        profile.add_line(record(iterations=200,
                                extra="poll 20 us/it wake 30 us/it (max 400) "))
        result = profile.result()
        self.assertEqual(result["iterations"], 300)
        self.assertEqual(result["frames"], 20)
        self.assertEqual(result["keys"], 12)
        self.assertEqual(result["sample_counts"]["wake"], 200)
        self.assertEqual(result["mean_us"]["poll"], 20)
        self.assertEqual(result["mean_us"]["wake"], 30)
        self.assertEqual(result["max_us"]["wake"], 400)

    def test_frame_weighting_excludes_idle(self):
        profile = analyzer.ProfileSummary()
        profile.add_line(record(iterations=1, frames=1, draw=1000, wait=100))
        profile.add_line(record(iterations=9, frames=9, draw=3000, wait=1000))
        self.assertEqual(profile.result()["mean_us"]["wait"], 910)
        profile.add_line(record(frames=0, draw=0))
        self.assertEqual(profile.result()["mean_us"]["draw"], 2800)
        self.assertEqual(profile.result()["frames"], 10)

    def test_idle_has_no_frame_cost(self):
        profile = analyzer.ProfileSummary()
        profile.add_line(record(frames=0))
        result = profile.result()
        self.assertIsNone(result["mean_us"]["draw"])
        self.assertIsNone(result["mean_us"]["present"])
        self.assertIsNone(result["max_us"]["draw"])

    def test_noise_ignored_but_broken_profile_rejected(self):
        profile = analyzer.ProfileSummary()
        self.assertFalse(profile.add_line("browser: BENCH draw 1234 us/frame"))
        with self.assertRaises(ValueError):
            profile.add_line("PROF iters 20 frames 1 keys 2 | wait 3")

    def test_expected_key_count_and_json(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "serial.log"
            log.write_text(record(keys=42), encoding="utf-8")
            output, errors = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                status = analyzer.main([str(log), "--expect-keys", "42", "--json"])
            self.assertEqual(status, 0)
            self.assertEqual(json.loads(output.getvalue())["keys"], 42)
            self.assertEqual(errors.getvalue(), "")
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(errors):
                status = analyzer.main([str(log), "--expect-keys", "43"])
            self.assertEqual(status, 1)
            self.assertIn("expected 43 keys, observed 42", errors.getvalue())

    def test_missing_records_is_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "empty.log"
            log.write_text("browser: startup\n", encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(analyzer.main([str(log)]), 2)


if __name__ == "__main__":
    unittest.main()
