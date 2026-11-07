#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Checks that geometry comparisons reject incomplete or incomparable tours."""

import unittest

from compare_geometry_logs import COMPARE_FIELDS, compare_logs, parse_geometry_log


def complete_log(**overrides):
    fields = dict(frames=1801, step_hz=30, simulated_seconds="60.032761",
                  total_tri=8100000, total_vtx=20250000,
                  peak_tri=6500, peak_vtx=16000, elapsed_us=42000000)
    fields.update(overrides)
    source = "Drift Los Angeles booting. Native PowerVR renderer.\n"
    for time, district in ((3, "DOWNTOWN CORE"), (18, "PACIFIC COAST"),
                           (33, "ARTS QUARTER"), (48, "NEON STRIP")):
        source += (f"Drift Los Angeles visual QA: t={time} district={district} "
                   "fps=45.0 reg=16.00ms tri=4500 vtx=11250.\n")
    source += "Drift Los Angeles geometry QA: complete; "
    source += " ".join(f"{key}={value}" for key, value in fields.items()) + ".\n"
    return source


class GeometryLogTests(unittest.TestCase):
    def test_equal_geometry_passes_with_different_elapsed_time(self):
        report = compare_logs(complete_log(), complete_log(elapsed_us=56000000))
        self.assertTrue(report["match"])
        self.assertEqual(report["failures"], [])
        self.assertEqual(len(report["baseline"]["districts_observed"]), 4)

    def test_every_required_comparison_field_rejects_a_change(self):
        values = dict(frames=1802, step_hz=31, simulated_seconds="60.032762",
                      total_tri=8100001, total_vtx=20250001,
                      peak_tri=6501, peak_vtx=16001)
        self.assertEqual(set(values), set(COMPARE_FIELDS))
        for field, value in values.items():
            with self.subTest(field=field):
                report = compare_logs(complete_log(), complete_log(**{field: value}))
                self.assertFalse(report["match"])
                self.assertTrue(any(field in failure for failure in report["failures"]))

    def test_missing_and_truncated_completion_rejected(self):
        full = complete_log()
        cases = (full.split("Drift Los Angeles geometry QA:")[0],
                 full.rstrip()[:-1], full.split(" peak_vtx=")[0],
                 full.replace(" elapsed_us=42000000", ""))
        for source in cases:
            with self.subTest(source=source[-60:]):
                self.assertFalse(compare_logs(full, source)["match"])

    def test_incomplete_final_launch_cannot_reuse_prior_completion(self):
        source = complete_log() + "Drift Los Angeles booting. Native PowerVR renderer.\n"
        self.assertIsNone(parse_geometry_log(source)["geometry"])
        self.assertFalse(compare_logs(source, source)["match"])

    def test_normal_benchmark_cannot_pass_as_geometry_run(self):
        source = complete_log() + "Drift Los Angeles benchmark: frames=1801.\n"
        self.assertTrue(any("normal benchmark" in failure
                            for failure in parse_geometry_log(source)["failures"]))

    def test_all_four_districts_must_precede_completion(self):
        full = complete_log()
        line = next(line for line in full.splitlines(True) if "NEON STRIP" in line)
        for source in (full.replace(line, ""), full.replace(line, "") + line):
            self.assertTrue(any("four-district" in failure
                                for failure in parse_geometry_log(source)["failures"]))

    def test_zero_negative_and_fractional_counts_rejected(self):
        for field, value in (("frames", 0), ("total_tri", 0), ("total_vtx", -1),
                             ("peak_tri", "6500.5"), ("elapsed_us", 0)):
            with self.subTest(field=field, value=value):
                self.assertTrue(parse_geometry_log(complete_log(**{field: value}))["failures"])

    def test_nonfinite_and_incomplete_simulated_duration_rejected(self):
        for seconds in ("nan", "inf", "1e999", "59.9", "61.0"):
            with self.subTest(seconds=seconds):
                self.assertTrue(parse_geometry_log(complete_log(simulated_seconds=seconds))["failures"])

    def test_duplicate_completion_and_fields_rejected(self):
        full = complete_log()
        completion = next(line for line in full.splitlines(True) if "geometry QA:" in line)
        for source in (full + completion, full.replace("step_hz=30", "step_hz=30 step_hz=30")):
            self.assertTrue(parse_geometry_log(source)["failures"])

    def test_impossible_whole_tour_totals_rejected(self):
        for values in (dict(total_tri=100), dict(total_vtx=40000000)):
            self.assertTrue(parse_geometry_log(complete_log(**values))["failures"])


if __name__ == "__main__":
    unittest.main()
