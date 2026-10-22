"""Portable static-verifier regressions; no game, compiler or target EXE needed."""

from pathlib import Path
import struct
import unittest

import verify_scale_hooks as verify


CREATE_PROLOGUE = bytes.fromhex("554157415641545657534881ec00010000")
APPEARANCE_PROLOGUE = bytes.fromhex("555657534881ecd8000000488dac2480000000")


def fixture():
    """Small authored PE fixture with distinct owners and four typed call sites."""
    data = bytearray(0x1600)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 2)
    struct.pack_into("<H", data, 0x94, 240)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<I", data, optional + 56, 0x3000)
    struct.pack_into("<I", data, optional + 108, 16)
    struct.pack_into("<II", data, optional + 112 + 3 * 8, 0x2000, 6 * 12)
    for i, (name, rva, raw, size, flags) in enumerate((
            (b".text", 0x1000, 0x400, 0x1000, 0x60000020),
            (b".pdata", 0x2000, 0x1400, 0x200, 0x40000040))):
        at = optional + 240 + 40 * i
        data[at:at + len(name)] = name
        struct.pack_into("<IIII", data, at + 8, size, rva, size, raw)
        struct.pack_into("<I", data, at + 36, flags)
    data[0x400:0x1400] = b"\x90" * 0x1000
    owners = [0x1100, 0x1200, 0x1400, 0x1500, 0x1600, 0x1700]
    for i, begin in enumerate(owners):
        struct.pack_into("<III", data, 0x1400 + 12 * i, begin, begin + 0x100, 0x2100)

    profile = {}

    def pattern(name, at, value, calls=()):
        mask = bytearray(b"\xff" * len(value))
        for offset in calls:
            mask[offset + 1:offset + 5] = bytes(4)
        profile[name + "Signature"] = bytes(value)
        profile[name + "SignatureMask"] = bytes(mask)
        write(data, at, value)

    for name, at, prologue in (("kCreateUnit", 0x1100, CREATE_PROLOGUE),
                               ("kAppearance", 0x1200, APPEARANCE_PROLOGUE)):
        profile[name + "Prologue"] = prologue
        pattern(name, at, prologue + name.encode())
    pattern("kCreateScale", 0x1180, b"authored-create-scale")
    pattern("kAppearanceScale", 0x1280, b"authored-appearance-scale")
    for role, owner in zip(("Boss", "Guardian", "Ordinary", "Ward"), owners[2:]):
        prefix = (role + "-context:").encode()
        at = owner + 0x10
        call = at + len(prefix)
        value = prefix + b"\xe8" + struct.pack("<i", 0x1100 - call - 5) + b":return"
        name = "k" + role + "CreateCall"
        pattern(name, at, value, (len(prefix),))
        profile[name + "Offset"] = len(prefix)
    return data, profile


def write(data, rva, value):
    offset = (0x400 + rva - 0x1000) if rva < 0x2000 else (0x1400 + rva - 0x2000)
    data[offset:offset + len(value)] = value


class RoleHookVerificationTests(unittest.TestCase):
    def test_complete_two_owner_four_role_contract(self):
        data, profile = fixture()
        result = verify.verify(verify.Image(data), profile)
        self.assertEqual(result["entries"]["CreateUnit"]["rva"], 0x1100)
        self.assertEqual(set(result["callers"]), {"Boss", "Guardian", "Ordinary", "Ward"})
        self.assertEqual(result["entries"]["CreateUnit"]["displaced_instruction_lengths"],
                         [1, 2, 2, 2, 1, 1, 1, 7])
        self.assertEqual(result["entries"]["Appearance"]["displaced_instruction_lengths"],
                         [1, 1, 1, 1, 7, 8])
        self.assertFalse(result["game_launched"])
        self.assertFalse(result["compiled"])

    def test_data_section_lookalike_is_not_a_hook(self):
        data, profile = fixture()
        write(data, 0x2080, profile["kCreateUnitSignature"])
        verify.verify(verify.Image(data), profile)

    def test_duplicate_executable_match_is_rejected(self):
        data, profile = fixture()
        write(data, 0x1800, profile["kCreateUnitSignature"])
        with self.assertRaisesRegex(verify.VerificationError, "found 2"):
            verify.verify(verify.Image(data), profile)

    def test_entry_without_unwind_boundary_is_rejected(self):
        data, profile = fixture()
        struct.pack_into("<I", data, 0x1400, 0x10FF)
        with self.assertRaisesRegex(verify.VerificationError, "No declared function begins"):
            verify.verify(verify.Image(data), profile)

    def test_truncated_or_overlapping_unwind_bounds_are_rejected(self):
        for end in (0x1100, 0x1201, 0x4000):
            with self.subTest(end=end):
                data, _ = fixture()
                struct.pack_into("<I", data, 0x1404, end)
                with self.assertRaises(verify.VerificationError):
                    verify.Image(data)

    def test_changed_masked_call_target_is_rejected(self):
        data, profile = fixture()
        at = 0x1410 + profile["kBossCreateCallOffset"]
        write(data, at + 1, struct.pack("<i", 0x1200 - at - 5))
        with self.assertRaisesRegex(verify.VerificationError, "Boss CALL does not reach"):
            verify.verify(verify.Image(data), profile)

    def test_new_unclassified_creator_call_is_rejected(self):
        data, profile = fixture()
        at = 0x1480
        write(data, at, b"\xe8" + struct.pack("<i", 0x1100 - at - 5))
        with self.assertRaisesRegex(verify.VerificationError, "Unclassified or missing"):
            verify.verify(verify.Image(data), profile)

    def test_scale_anchor_in_other_owner_is_rejected(self):
        data, profile = fixture()
        value = profile["kCreateScaleSignature"]
        write(data, 0x1180, b"\x90" * len(value))
        write(data, 0x1380, value)
        with self.assertRaisesRegex(verify.VerificationError, "outside its expected native owner"):
            verify.verify(verify.Image(data), profile)

    def test_role_context_outside_unwind_owner_is_rejected(self):
        data, profile = fixture()
        name = "kWardCreateCallSignature"
        value = profile[name]
        write(data, 0x1710, b"\x90" * len(value))
        at = 0x1810
        call = at + profile["kWardCreateCallOffset"]
        moved = bytearray(value)
        struct.pack_into("<i", moved, profile["kWardCreateCallOffset"] + 1, 0x1100 - call - 5)
        write(data, at, moved)
        with self.assertRaisesRegex(verify.VerificationError, "No declared function contains"):
            verify.verify(verify.Image(data), profile)

    def test_unsafe_or_incomplete_displaced_instructions_are_rejected(self):
        for value in (b"\x90" * 14, CREATE_PROLOGUE[:-1],
                      CREATE_PROLOGUE + b"\xe8\0\0\0\0",
                      CREATE_PROLOGUE + bytes.fromhex("488b0500000000"),
                      CREATE_PROLOGUE + b"\x41"):
            with self.subTest(prologue=value.hex()):
                with self.assertRaises(verify.VerificationError):
                    verify.instruction_lengths(value)

    def test_missing_or_weak_signature_masks_are_rejected(self):
        data, _ = fixture()
        image = verify.Image(data)
        for signature, mask in ((b"x", b""), (b"x", b"\0"), (b"x", b"\x7f")):
            with self.subTest(mask=mask):
                with self.assertRaises(verify.VerificationError):
                    image.matches(signature, mask)

    def test_truncated_headers_and_sections_are_rejected(self):
        data, _ = fixture()
        for size in (0, 2, 0x40, 0x90, 0x200, len(data) - 1):
            with self.subTest(size=size):
                with self.assertRaises(verify.VerificationError):
                    verify.Image(data[:size])

    def test_profile_masks_only_reviewed_calls_and_non_abi_operands(self):
        path = Path(__file__).resolve().parents[1] / "include" / "target_profile.hpp"
        profile = verify.header_profile(path)
        calls = {"CreateUnit": [107], "Appearance": [88], "BossCreateCall": [49],
                 "GuardianCreateCall": [46], "OrdinaryCreateCall": [50],
                 "WardCreateCall": [34, 53], "CreateScale": [], "AppearanceScale": [16]}
        non_abi = {"CreateUnit": [(62, 66)],
                   "GuardianCreateCall": [(8, 9), (57, 61)],
                   "OrdinaryCreateCall": [(59, 63)]}
        for owner, offsets in calls.items():
            with self.subTest(owner=owner):
                signature = profile["k" + owner + "Signature"]
                expected = bytearray(b"\xff" * len(signature))
                for offset in offsets:
                    self.assertEqual(signature[offset], 0xE8)
                    expected[offset + 1:offset + 5] = bytes(4)
                for begin, end in non_abi.get(owner, []):
                    expected[begin:end] = bytes(end - begin)
                self.assertEqual(profile["k" + owner + "SignatureMask"], bytes(expected))
                self.assertTrue(all(match or byte == 0 for byte, match in zip(signature, expected)),
                                "Masked signature bytes must be zero for the runtime matcher")

    def test_old_and_updated_non_abi_operands_match_the_same_profile(self):
        path = Path(__file__).resolve().parents[1] / "include" / "target_profile.hpp"
        profile = verify.header_profile(path)
        # Authored copies of the bounded public profiles vary only operands
        # unrelated to the hook ABI: a Game-field displacement, caller stack
        # local and post-call Game-field displacement. No EXE fixture is needed.
        changes = {"CreateUnit": [{62: bytes.fromhex("e8040000")},
                                  {62: bytes.fromhex("e0040000")}],
                   "GuardianCreateCall": [{8: b"\x18", 57: bytes.fromhex("540a0000")},
                                          {8: b"\x20", 57: bytes.fromhex("4c0a0000")}],
                   "OrdinaryCreateCall": [{59: bytes.fromhex("540a0000")},
                                          {59: bytes.fromhex("4c0a0000")}]}
        for owner, versions in changes.items():
            signature = profile["k" + owner + "Signature"]
            mask = profile["k" + owner + "SignatureMask"]
            for version, operands in enumerate(versions):
                with self.subTest(owner=owner, version=version):
                    actual = bytearray(signature)
                    for offset, value in operands.items():
                        actual[offset:offset + len(value)] = value
                    data, _ = fixture()
                    write(data, 0x1800, actual)
                    self.assertEqual(verify.Image(data).unique(signature, mask), 0x1800)

    def test_abi_argument_and_opcode_changes_are_still_rejected(self):
        path = Path(__file__).resolve().parents[1] / "include" / "target_profile.hpp"
        profile = verify.header_profile(path)
        # Creator register captures select R9d display and R8d entry; caller
        # offsets select the display ID and the fifth argument's scale slot.
        retained = {"CreateUnit": [0, 39, 40, 43, 54],
                    "GuardianCreateCall": [0, 4, 34],
                    "OrdinaryCreateCall": [0, 9, 36]}
        for owner, offsets in retained.items():
            signature = profile["k" + owner + "Signature"]
            mask = profile["k" + owner + "SignatureMask"]
            for offset in offsets:
                with self.subTest(owner=owner, offset=offset):
                    self.assertEqual(mask[offset], 255)
                    actual = bytearray(signature)
                    actual[offset] ^= 1
                    data, _ = fixture()
                    write(data, 0x1800, actual)
                    self.assertEqual(verify.Image(data).matches(signature, mask), [])

    def test_committed_profile_has_complete_reviewed_prologues(self):
        path = Path(__file__).resolve().parents[1] / "include" / "target_profile.hpp"
        profile = verify.header_profile(path)
        self.assertEqual(profile["kCreateUnitPrologue"], CREATE_PROLOGUE)
        self.assertEqual(profile["kAppearancePrologue"], APPEARANCE_PROLOGUE)
        for name in ("kCreateUnit", "kAppearance"):
            self.assertEqual(sum(verify.instruction_lengths(profile[name + "Prologue"])),
                             len(profile[name + "Prologue"]))


if __name__ == "__main__":
    unittest.main()
