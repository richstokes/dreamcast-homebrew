#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Regression checks for geometry seams and trustworthy render-log gates."""

from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from analyze_render_log import analyze_log
from PIL import Image
from visual_qa import HUD_BYTES, audit_textures, evaluate, parse_model


BENCHMARK = (
    "Drift Los Angeles benchmark: frames={frames} elapsed_us={elapsed} "
    "average_fps={fps} peak_tri=6500 peak_vtx=13000 texture_bytes=2785280 "
    "vram_free=2097152 vertex_buffer_bytes=786432.\n"
)


class BenchmarkTests(unittest.TestCase):
    def test_whole_run_count_controls_result(self):
        source = (
            "Drift Los Angeles visual QA: t=3 district=Downtown Core "
            "fps=59.8 reg=2.00ms tri=6000 vtx=12000.\n"
            + BENCHMARK.format(frames=1740, elapsed=60000000, fps="29.00")
        )
        result = analyze_log(source)
        self.assertEqual(result["benchmark"]["average_fps"], 29.0)
        self.assertTrue(any("below 30.00 FPS" in failure for failure in result["failures"]))

    def test_thirty_fps_passes_with_actual_runtime_inventory(self):
        result = analyze_log(BENCHMARK.format(frames=1800, elapsed=60000000, fps="30.00"))
        self.assertEqual(result["failures"], [])
        self.assertEqual(result["benchmark"]["vertex_stream_lower_bound_bytes"], 416000)

    def test_old_samples_cannot_establish_whole_run_average(self):
        result = analyze_log("Drift Los Angeles showcase: t=3 district=Neon Strip fps=60.0 reg=1.0ms.")
        self.assertEqual(result["districts_observed"], ["Neon Strip"])
        self.assertIsNone(result["benchmark"])
        self.assertTrue(result["failures"])

    def test_new_incomplete_launch_does_not_reuse_old_result(self):
        source = BENCHMARK.format(frames=1800, elapsed=60000000, fps="30.00")
        result = analyze_log(source + "Drift Los Angeles booting. Native PowerVR renderer.\n")
        self.assertIsNone(result["benchmark"])
        self.assertTrue(result["failures"])

    def test_claimed_fps_and_short_duration_do_not_bypass_gate(self):
        result = analyze_log(BENCHMARK.format(frames=60, elapsed=2000000, fps="60.00"))
        self.assertTrue(any("disagrees" in failure for failure in result["failures"]))
        self.assertTrue(any("duration" in failure for failure in result["failures"]))


class MeshTests(unittest.TestCase):
    def read_fixture(self, uv_outside: bool = False, bad_ranges: bool = False,
                     shared_material: bool = False):
        # The two triangles meet at a UV/hard-normal seam but share no indices.
        vertices = [
            (-1, 0, -2, 0, 0), (0, 0, 0, .5, 0), (0, 1, 2, .5, 1),
            (0, 0, 0, .5, 0), (1, 0, -2, 1, 0), (0, 1, 2, .5, 1),
        ]
        if uv_outside:
            vertices[0] = (*vertices[0][:3], 1.5, 0)
        rows = ["{" + ",".join(map(str, (*vertex, 0, 1, 0, .8))) + "}"
                for vertex in vertices]
        source = "static const dla_mesh_vertex_t dla_car_vertices[] = {\n"
        source += ",\n".join(rows) + "\n};\n"
        second_face = "{1,4,2,1}" if shared_material else "{3,4,5,0}"
        source += "static const dla_mesh_face_t dla_car_faces[] = {{0,1,2,0}," + second_face + "};\n"
        ranges = "{0,1},{1,1},{2,0},{2,0},{2,0}" if bad_ranges or shared_material else "{0,2},{2,0},{2,0},{2,0},{2,0}"
        source += "static const dla_mesh_material_range_t dla_car_material_ranges[5] = {" + ranges + "};\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "model_data.h"
            path.write_text(source)
            return parse_model(path)

    def test_split_vertices_preserve_geometric_body_connectivity(self):
        result = self.read_fixture()
        self.assertEqual(result["vertices"], 6)
        self.assertEqual(result["body_width"], 2.0)
        self.assertEqual(result["body_length"], 4.0)
        self.assertEqual(result["invalid_uvs"], 0)
        self.assertEqual(result["collapsed_uv_triangles_by_material"], [0] * 5)
        self.assertFalse(result["invalid_material_ranges"])
        self.assertEqual(result["shared_material_vertices"], 0)

    def test_atlas_overrun_is_reported(self):
        self.assertEqual(self.read_fixture(uv_outside=True)["invalid_uvs"], 1)

    def test_material_range_cannot_claim_other_material_faces(self):
        self.assertTrue(self.read_fixture(bad_ranges=True)["invalid_material_ranges"])

    def test_different_materials_cannot_reuse_a_cached_shading_vertex(self):
        model = self.read_fixture(shared_material=True)
        self.assertEqual(model["shared_material_vertices"], 2)
        self.assertFalse(model["invalid_material_ranges"])
        failures = evaluate({"model": model, "textures": audit_textures(), "captures": []})
        self.assertTrue(any("cached shading" in failure for failure in failures))

    def test_face_count_is_bounded_by_visibility_cache(self):
        model = self.read_fixture()
        model["triangles"] = 4097
        failures = evaluate({"model": model, "textures": audit_textures(), "captures": []})
        self.assertEqual(model["renderer_face_capacity"], 4096)
        self.assertTrue(any("4,096-face visibility" in failure for failure in failures))


class TextureInventoryTests(unittest.TestCase):
    def test_mip_levels_and_transfer_padding_are_included(self):
        with tempfile.TemporaryDirectory() as directory:
            source_dir = Path(directory)
            Image.new("RGBA", (8, 8)).save(source_dir / "test.png")
            texture = SimpleNamespace(name="test", source="test.png", size=8,
                                      mipmap=True, alpha=True)
            with patch("visual_qa.SOURCE_DIR", source_dir), \
                 patch("visual_qa.load_texture_specs", return_value=(texture,)):
                result = audit_textures()
        # 3 leading words + (1+4+16+64) texels = 176 bytes, padded to 192.
        self.assertEqual(result["pvr_bytes_including_hud"], HUD_BYTES + 192)
        self.assertEqual(result["mipmapped_textures"], ["test"])
        self.assertEqual(result["alpha_textures"], ["test"])

    def test_nonsquare_mipmap_is_rejected(self):
        texture = SimpleNamespace(name="test", source="missing.png", size=(16, 8),
                                  mipmap=True, alpha=False)
        with patch("visual_qa.load_texture_specs", return_value=(texture,)):
            result = audit_textures()
        self.assertTrue(result["invalid_output_dimensions"])


if __name__ == "__main__":
    unittest.main()
