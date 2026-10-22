"""Standard-library tests: synthetic assets contain no game data."""

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

SPEC = importlib.util.spec_from_file_location("prepare", Path(__file__).with_name("prepare_crab_assets.py"))
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


def fake_model():
    model = bytearray(0x80)
    model[:4] = b"MD20"
    struct.pack_into("<I", model, 4, 256)
    struct.pack_into("<II", model, 0x5C, 1, 0x70)
    name = b"Creature\\Crab\\crabreflect.blp\0"
    struct.pack_into("<4I", model, 0x70, 0, 0, len(name), len(model))
    return bytes(model) + name


class AssetTests(unittest.TestCase):
    def setUp(self):
        # Keep disposable fixture files inside the checkout, matching repo rules.
        runtime = Path(__file__).resolve().parents[1] / "runtime"
        runtime.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="asset-tests-", dir=runtime)
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_storm_hash_reference_vectors(self):
        self.assertEqual(prepare.storm_hash("(hash table)", 0x300), 0xC3AF3770)
        self.assertEqual(prepare.storm_hash("(block table)", 0x300), 0xEC83B3A3)
        self.assertEqual(prepare.storm_hash("file.txt", 0), 0x3EA98D7A)

    def test_extract_complete_crab_archive(self):
        files = {prepare.CRAB_MODEL: fake_model(), r"Creature\Crab\crabreflect.blp": b"BLP1test-reflection"}
        files.update({rf"Creature\Crab\CrabSkin{skin}.blp": b"BLP1test-" + skin.encode() for skin in prepare.SKINS})
        source, output = self.directory / "source.MPQ", self.directory / "patch-Z.MPQ"
        source.write_bytes(prepare.make_mpq(files))
        manifest = prepare.prepare([source], output)
        self.assertEqual(len(manifest["files"]), 6)
        archive = prepare.Mpq(output)
        for name, expected in files.items():
            self.assertEqual(archive.read(name.replace("\\", "/").lower()), expected)
        self.assertIsNone(archive.read("no-such-file"))
        self.assertIn(b"Creature\\Crab\\Crab.m2", archive.read("(listfile)"))

    def test_zlib_sector_and_single_unit(self):
        original = b"repeated-asset" * 500
        compressed = b"\x02" + zlib.compress(original)
        for single in (False, True):
            with self.subTest(single=single):
                archive = bytearray(prepare.make_mpq({"asset": original}))
                # Rebuild one compressed member with the same real hash tables.
                _, _, _, _, _, hpos, bpos, nhash, nblock = struct.unpack_from("<IIIHHIIII", archive)
                hashes = prepare.decrypt_table(archive[hpos:bpos], prepare.storm_hash("(hash table)", 0x300))
                blocks = bytearray(prepare.decrypt_table(archive[bpos:bpos + nblock * 16], prepare.storm_hash("(block table)", 0x300)))
                # Sector size is 4096, therefore use two independently compressed sectors.
                if single:
                    payload = compressed
                else:
                    chunks = [b"\x02" + zlib.compress(original[i:i + 4096]) for i in range(0, len(original), 4096)]
                    offsets = [4 * (len(chunks) + 1)]
                    for chunk in chunks:
                        offsets.append(offsets[-1] + len(chunk))
                    payload = struct.pack(f"<{len(offsets)}I", *offsets) + b"".join(chunks)
                flags = 0x80000200 | (0x1000000 if single else 0)
                struct.pack_into("<4I", blocks, 0, 32, len(payload), len(original), flags)
                # Keep the stored listfile from the original second block.
                listpos, listsize, _, _ = struct.unpack_from("<4I", blocks, 16)
                listfile = archive[listpos:listpos + listsize]
                struct.pack_into("<I", blocks, 16, 32 + len(payload))
                hp = 32 + len(payload) + len(listfile)
                bp = hp + len(hashes)
                header = struct.pack("<IIIHHIIII", 0x1A51504D, 32, bp + len(blocks), 0, 3, hp, bp, nhash, nblock)
                rebuilt = header + payload + listfile + prepare.encrypt_table(hashes, prepare.storm_hash("(hash table)", 0x300)) + prepare.encrypt_table(bytes(blocks), prepare.storm_hash("(block table)", 0x300))
                path = self.directory / f"compressed-{single}.MPQ"
                path.write_bytes(rebuilt)
                self.assertEqual(prepare.Mpq(path).read("asset"), original)

    def test_missing_assets_leave_no_output(self):
        source = self.directory / "trimmed.MPQ"
        source.write_bytes(prepare.make_mpq({"unrelated": b"x"}))
        output = self.directory / "patch-Z.MPQ"
        with self.assertRaisesRegex(prepare.AssetError, "missing Creature"):
            prepare.prepare([source], output)
        self.assertFalse(output.exists())

    def test_unsafe_dependencies_and_newer_models_rejected(self):
        for name in (r"..\Creature\Crab\a.blp", r"C:\Creature\Crab\a.blp", r"Creature\Crab\..\evil.blp", r"Other\a.blp"):
            with self.subTest(name=name), self.assertRaises(prepare.AssetError):
                prepare.safe_member(name)
        newer = bytearray(fake_model())
        struct.pack_into("<I", newer, 4, 264)
        with self.assertRaisesRegex(prepare.AssetError, "Vanilla"):
            prepare.texture_dependencies(newer)

    def test_truncated_tables_and_codec_rejected(self):
        path = self.directory / "broken.MPQ"
        path.write_bytes(prepare.make_mpq({"asset": b"abc"})[:-1])
        with self.assertRaises(prepare.AssetError):
            prepare.Mpq(path)
        with self.assertRaisesRegex(prepare.AssetError, "unsupported MPQ compression"):
            prepare._inflate(b"\x08pkware", 500)


if __name__ == "__main__":
    unittest.main()
