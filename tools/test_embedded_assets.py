"""Validate the actual bundled binaries without building or launching the game."""

import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets" / "crab"
SPEC = importlib.util.spec_from_file_location("asset_parser", Path(__file__).with_name("prepare_crab_assets.py"))
parser = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(parser)


class EmbeddedAssetTests(unittest.TestCase):
    def test_manifest_and_resource_payloads(self):
        manifest = json.loads((ASSETS / "manifest.json").read_text())
        rows = manifest["files"]
        self.assertEqual(len(rows), 6)
        names = {row["name"] for row in rows}
        self.assertEqual(len(names), 6)
        for row in rows:
            with self.subTest(asset=row["name"]):
                data = (ASSETS / row["name"]).read_bytes()
                self.assertEqual(len(data), row["bytes"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), row["sha256"])
        resources = re.findall(r'^([0-9]+) RCDATA "@CRAB_ASSET_DIR@/([^"]+)"',
                               (ROOT / "resources" / "crab_assets.rc.in").read_text(), re.M)
        self.assertEqual(len(resources), 6)
        self.assertEqual(len({resource for resource, _ in resources}), 6)
        self.assertEqual({name for _, name in resources}, names)
        self.assertEqual(sum(row["bytes"] for row in rows), 596644)

    def test_model_dependency_closure_and_native_skin_slot(self):
        model = (ASSETS / "Crab.m2").read_bytes()
        manifest = json.loads((ASSETS / "manifest.json").read_text())
        available = {row["mpq_path"].replace("/", "\\").lower() for row in manifest["files"]}
        dependencies = parser.texture_dependencies(model)
        self.assertTrue(dependencies)
        self.assertTrue(all(path.lower() in available for path in dependencies))
        count, at = struct.unpack_from("<II", model, 0x5C)
        types = [struct.unpack_from("<I", model, at + i * 16)[0] for i in range(count)]
        self.assertIn(11, types, "Monster1 must select the randomized body skin")
        skin_hashes = {hashlib.sha256((ASSETS / f"CrabSkin{skin}.blp").read_bytes()).digest()
                       for skin in parser.SKINS}
        self.assertEqual(len(skin_hashes), 4, "The four color variants must be distinct")

    def test_texture_mipmaps_are_complete_and_bounded(self):
        for path in ASSETS.glob("*.blp"):
            with self.subTest(texture=path.name):
                data = path.read_bytes()
                self.assertEqual(data[:4], b"BLP2")
                self.assertEqual(data[8], 2, "Expected the source's DXT texture encoding")
                self.assertIn(data[10], (0, 1, 7))
                width, height = struct.unpack_from("<II", data, 12)
                self.assertGreater(width, 0)
                self.assertGreater(height, 0)
                offsets = struct.unpack_from("<16I", data, 20)
                sizes = struct.unpack_from("<16I", data, 84)
                levels = max(width, height).bit_length()
                self.assertEqual(sum(bool(size) for size in sizes), levels)
                block = 8 if data[10] == 0 else 16
                previous_end = 148
                for mip in range(levels):
                    w, h = max(1, width >> mip), max(1, height >> mip)
                    self.assertEqual(sizes[mip], ((w + 3) // 4) * ((h + 3) // 4) * block)
                    self.assertGreaterEqual(offsets[mip], previous_end)
                    previous_end = offsets[mip] + sizes[mip]
                    self.assertLessEqual(previous_end, len(data))


if __name__ == "__main__":
    unittest.main()
