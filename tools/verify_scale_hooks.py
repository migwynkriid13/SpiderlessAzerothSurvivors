#!/usr/bin/env python3
"""Inspect optional local AMD64 reference bytes without building or launching.

Usage: python -B tools/verify_scale_hooks.py /path/to/AzerothSurvivors.exe
The supplied EXE stays read-only. No target path, game hash or generated report
is needed by ordinary tests or the DLL. This is independent static evidence,
not a compatibility guarantee for future game versions or a runtime test.
"""

import argparse
import json
from pathlib import Path
import re
import struct


class VerificationError(ValueError):
    """The supplied image does not establish the expected native contract."""


def require(condition, message):
    if not condition:
        raise VerificationError(message)


class Image:
    """Minimal bounded PE32+ reader; offsets refer to the file, results to RVAs."""

    def __init__(self, data):
        self.data = data
        require(len(data) >= 0x40 and data[:2] == b"MZ", "Missing DOS header")
        pe = self.integer(0x3C)
        require(self.read(pe, 4) == b"PE\0\0", "Missing PE signature")
        require(self.integer(pe + 4, "H") == 0x8664, "Expected AMD64 image")
        count = self.integer(pe + 6, "H")
        optional_size = self.integer(pe + 20, "H")
        optional = pe + 24
        require(optional_size >= 144, "Truncated PE32+ optional header")
        self.read(optional, optional_size)
        require(self.integer(optional, "H") == 0x20B, "Expected PE32+ image")
        self.image_base = self.integer(optional + 24, "Q")
        self.image_size = self.integer(optional + 56)
        require(self.image_size > 0, "Empty mapped image")
        self.sections = []
        for i in range(count):
            at = optional + optional_size + 40 * i
            self.read(at, 40)
            virtual_size, rva, size, raw = struct.unpack_from("<IIII", data, at + 8)
            flags = self.integer(at + 36)
            require(rva + max(virtual_size, size) <= self.image_size,
                    "Section exceeds mapped image")
            self.read(raw, size)
            self.sections.append((rva, size, raw, bool(flags & 0x20000000)))
        require(self.integer(optional + 108) > 3, "Missing exception directory")
        table_rva, table_size = struct.unpack("<II", self.read(optional + 112 + 3 * 8, 8))
        require(table_size > 0 and table_size % 12 == 0, "Invalid exception directory size")
        table = self.read_rva(table_rva, table_size)
        self.functions = []
        previous_end = 0
        for i in range(0, len(table), 12):
            begin, end, unwind = struct.unpack_from("<III", table, i)
            require(previous_end <= begin < end <= self.image_size,
                    "Invalid or overlapping exception function bounds")
            require(self.executable(begin, end - begin), "Function is outside executable sections")
            require(unwind != 0, "Missing unwind metadata")
            self.functions.append((begin, end))
            previous_end = end

    def read(self, offset, size):
        require(offset >= 0 and size >= 0 and offset <= len(self.data) - size,
                "Read exceeds file bounds")
        return self.data[offset:offset + size]

    def integer(self, offset, kind="I"):
        return struct.unpack("<" + kind, self.read(offset, struct.calcsize("<" + kind)))[0]

    def read_rva(self, rva, size):
        require(rva >= 0 and size >= 0, "Negative mapped range")
        for start, length, raw, _ in self.sections:
            if start <= rva and rva + size <= start + length:
                return self.read(raw + rva - start, size)
        raise VerificationError(f"Unbacked mapped range at RVA {rva:#x}")

    def executable(self, rva, size):
        return any(execute and start <= rva and rva + size <= start + length
                   for start, length, _, execute in self.sections)

    def matches(self, signature, mask):
        require(len(signature) == len(mask) and len(signature) > 0, "Invalid signature length")
        require(all(value in (0, 255) for value in mask), "Unsupported signature mask")
        runs = [(match.start(), match.end()) for match in re.finditer(b"\xff+", mask)]
        require(runs, "Signature must contain exact bytes")
        first, last = max(runs, key=lambda run: run[1] - run[0])
        needle = signature[first:last]
        found = []
        for start, size, raw, execute in self.sections:
            if not execute:
                continue
            section = self.read(raw, size)
            at = section.find(needle)
            while at >= 0:
                candidate = at - first
                if (candidate >= 0 and candidate + len(signature) <= size and
                        all(not match or section[candidate + i] == signature[i]
                            for i, match in enumerate(mask))):
                    found.append(start + candidate)
                at = section.find(needle, at + 1)
        return found

    def unique(self, signature, mask):
        matches = self.matches(signature, mask)
        require(len(matches) == 1, f"Expected one executable signature match; found {len(matches)}")
        return matches[0]

    def function_end(self, begin):
        for start, end in self.functions:
            if start == begin:
                return end
        raise VerificationError(f"No declared function begins at RVA {begin:#x}")

    def call_target(self, call):
        instruction = self.read_rva(call, 5)
        require(instruction[0] == 0xE8, f"Expected direct CALL at RVA {call:#x}")
        target = call + 5 + struct.unpack_from("<i", instruction, 1)[0]
        require(self.executable(target, 1), "CALL target is outside executable sections")
        return target


def instruction_lengths(prologue):
    """Decode only the position-independent instructions allowed in our stubs.

    This deliberately small allowlist rejects branches, calls, RIP-relative
    addressing, partial instructions and new prologue forms until reviewed.
    It is not a general x64 decoder.
    """
    at = 0
    lengths = []
    while at < len(prologue):
        begin = at
        rex = 0
        if 0x40 <= prologue[at] <= 0x4F:
            rex = prologue[at]
            at += 1
            require(at < len(prologue), "Incomplete REX prefix")
        opcode = prologue[at]
        at += 1
        if 0x50 <= opcode <= 0x57:
            pass  # PUSH register (including a REX-extended register).
        elif opcode in (0x89, 0x8B):
            require(at < len(prologue) and prologue[at] & 0xC0 == 0xC0,
                    "Only register MOV is allowed in a displaced prologue")
            at += 1
        elif opcode in (0x81, 0x83):
            require(rex == 0x48 and at < len(prologue) and prologue[at] == 0xEC,
                    "Only SUB RSP is allowed in a displaced prologue")
            at += 1 + (4 if opcode == 0x81 else 1)
        elif opcode == 0x8D:
            # LEA RBP,[RSP+disp8] or LEA RBP,[RSP+disp32].
            require(rex == 0x48 and at + 2 <= len(prologue), "Invalid frame LEA")
            modrm, sib = prologue[at:at + 2]
            require(modrm in (0x6C, 0xAC) and sib == 0x24, "Unsafe frame LEA")
            at += 2 + (1 if modrm == 0x6C else 4)
        else:
            raise VerificationError(f"Unreviewed displaced opcode {opcode:#x}")
        require(at <= len(prologue), "Incomplete displaced instruction")
        lengths.append(at - begin)
    require(len(prologue) >= 14, "Displaced prologue is shorter than the absolute detour")
    return lengths


def header_profile(path):
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", path.read_text(), flags=re.S)
    arrays = {}
    for count, name, body in re.findall(
            r"std::array<std::uint8_t,\s*(\d+)>\s+(\w+)\s*\{([^}]+)\}", text):
        values = [int(word, 0) for word in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", body)]
        require(len(values) == int(count) and all(0 <= word <= 255 for word in values),
                f"Invalid signature array {name}")
        arrays[name] = bytes(values)
    for name, value in re.findall(r"std::size_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;", text):
        arrays[name] = int(value, 0)
    return arrays


def verify_entry(image, arrays, name):
    signature = arrays[name + "Signature"]
    mask = arrays[name + "SignatureMask"]
    prologue = arrays[name + "Prologue"]
    lengths = instruction_lengths(prologue)
    entry = image.unique(signature, mask)
    end = image.function_end(entry)
    require(end - entry >= len(signature), "Signature extends beyond declared function")
    require(image.read_rva(entry, len(prologue)) == prologue, "Displaced prologue differs")
    return {"rva": entry, "end_rva": end, "displaced_instruction_lengths": lengths}


def function_containing(image, begin, size):
    for start, end in image.functions:
        if start <= begin and begin + size <= end:
            return start, end
    raise VerificationError(f"No declared function contains RVA {begin:#x}")


def signature(image, arrays, name):
    return image.unique(arrays[name + "Signature"], arrays[name + "SignatureMask"])


def creator_calls(image, creator):
    calls = set()
    for start, size, raw, execute in image.sections:
        if not execute:
            continue
        data = image.read(raw, size)
        at = data.find(b"\xe8")
        while 0 <= at <= len(data) - 5:
            displacement = struct.unpack_from("<i", data, at + 1)[0]
            if start + at + 5 + displacement == creator:
                calls.add(start + at)
            at = data.find(b"\xe8", at + 1)
    return calls


def verify(image, arrays):
    entries = {name: verify_entry(image, arrays, "k" + name)
               for name in ("CreateUnit", "Appearance")}
    for owner, name in (("CreateUnit", "kCreateScale"), ("Appearance", "kAppearanceScale")):
        at = signature(image, arrays, name)
        require(entries[owner]["rva"] <= at and
                at + len(arrays[name + "Signature"]) <= entries[owner]["end_rva"],
                f"{name} is outside its expected native owner")
        entries[owner]["scale_anchor_rva"] = at

    callers = {}
    for role in ("Boss", "Guardian", "Ordinary", "Ward"):
        name = "k" + role + "CreateCall"
        at = signature(image, arrays, name)
        begin, end = function_containing(image, at, len(arrays[name + "Signature"]))
        offset = arrays[name + "Offset"]
        require(0 <= offset <= len(arrays[name + "Signature"]) - 5,
                f"{role} CALL offset exceeds its context")
        call = at + offset
        require(image.call_target(call) == entries["CreateUnit"]["rva"],
                f"{role} CALL does not reach the discovered creator")
        callers[role] = {"context_rva": at, "call_rva": call,
                         "owner_rva": begin, "owner_end_rva": end}
    expected = {caller["call_rva"] for caller in callers.values()}
    require(len(expected) == 4, "Role CALL contexts overlap")
    require(creator_calls(image, entries["CreateUnit"]["rva"]) == expected,
            "Unclassified or missing direct creator CALL")
    return {"entries": entries, "callers": callers,
            "game_launched": False, "compiled": False,
            "limits": "Static native-contract evidence; gameplay and rendered sizes remain unverified."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path, help="Read-only local Windows x64 game executable")
    parser.add_argument("--profile", type=Path,
                        default=Path(__file__).resolve().parents[1] / "include" / "target_profile.hpp")
    args = parser.parse_args()
    try:
        result = verify(Image(args.exe.read_bytes()), header_profile(args.profile))
    except (OSError, VerificationError, KeyError) as error:
        parser.exit(1, f"Scale-hook verification failed: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
