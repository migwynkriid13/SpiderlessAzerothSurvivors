#!/usr/bin/env python3
"""Extract locally owned Vanilla crab assets; never downloads or bundles game data.

Supports ordinary MPQ v0/v1 archives at file offset zero, raw and zlib sectors.
Encrypted members, PKWARE/other codecs, delta patches and newer formats fail
explicitly. The Storm hash/decryption algorithm and pre-Wrath M2 texture layout
are implemented directly; no third-party Python packages are required.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import struct
import sys
import zlib

MASK = 0xFFFFFFFF
MAX_MEMBER = 32 * 1024 * 1024
MAX_TABLE = 16 * 1024 * 1024
CRAB_MODEL = r"Creature\Crab\Crab.m2"
SKINS = ("Bronze", "Ivory", "Saphire", "Vermillian")


class AssetError(ValueError):
    pass


def _crypt_table() -> list[int]:
    table = [0] * 0x500
    seed = 0x100001
    for byte in range(256):
        for kind in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            high = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[byte + kind * 256] = high | (seed & 0xFFFF)
    return table


CRYPT = _crypt_table()


def storm_hash(name: str, kind: int) -> int:
    first, second = 0x7FED7FED, 0xEEEEEEEE
    for byte in name.replace("/", "\\").upper().encode("ascii"):
        first = CRYPT[kind + byte] ^ ((first + second) & MASK)
        second = (byte + first + second + (second << 5) + 3) & MASK
    return first


def decrypt_table(raw: bytes, key: int) -> bytes:
    if len(raw) % 4:
        raise AssetError("unaligned MPQ table")
    seed, result = 0xEEEEEEEE, bytearray()
    for (encrypted,) in struct.iter_unpack("<I", raw):
        seed = (seed + CRYPT[0x400 + (key & 255)]) & MASK
        word = encrypted ^ ((key + seed) & MASK)
        result.extend(struct.pack("<I", word))
        key = ((((~key) << 21) + 0x11111111) | (key >> 11)) & MASK
        seed = (word + seed + (seed << 5) + 3) & MASK
    return bytes(result)


def encrypt_table(raw: bytes, key: int) -> bytes:
    seed, result = 0xEEEEEEEE, bytearray()
    for (word,) in struct.iter_unpack("<I", raw):
        seed = (seed + CRYPT[0x400 + (key & 255)]) & MASK
        result.extend(struct.pack("<I", word ^ ((key + seed) & MASK)))
        key = ((((~key) << 21) + 0x11111111) | (key >> 11)) & MASK
        seed = (word + seed + (seed << 5) + 3) & MASK
    return bytes(result)


def make_mpq(files: dict[str, bytes]) -> bytes:
    """Write classic MPQ format 0, stored members, encrypted directory tables."""
    members = {**files}
    members["(listfile)"] = ("\r\n".join([*files, "(listfile)"]) + "\r\n").encode("ascii")
    slots = 4
    while slots < len(members) * 2:
        slots *= 2
    hashes = bytearray(b"\xff" * (slots * 16))
    blocks, payload = bytearray(), bytearray()
    for index, (name, raw) in enumerate(members.items()):
        if len(raw) > MAX_MEMBER:
            raise AssetError("excessive MPQ output member")
        position = 32 + len(payload)
        payload.extend(raw)
        blocks.extend(struct.pack("<4I", position, len(raw), len(raw), 0x80000000))
        slot = storm_hash(name, 0) % slots
        while struct.unpack_from("<I", hashes, slot * 16 + 12)[0] != MASK:
            slot = (slot + 1) % slots
        struct.pack_into("<IIHHI", hashes, slot * 16, storm_hash(name, 0x100), storm_hash(name, 0x200), 0, 0, index)
    payload.extend(b"\0" * ((-len(payload)) % 4))
    hash_position = 32 + len(payload)
    block_position = hash_position + len(hashes)
    size = block_position + len(blocks)
    header = struct.pack("<IIIHHIIII", 0x1A51504D, 32, size, 0, 3, hash_position, block_position, slots, len(members))
    return header + payload + encrypt_table(bytes(hashes), storm_hash("(hash table)", 0x300)) + encrypt_table(bytes(blocks), storm_hash("(block table)", 0x300))


def _inflate(raw: bytes, expected: int) -> bytes:
    if len(raw) == expected:
        return raw
    if not raw or raw[0] != 2:
        codec = f"0x{raw[0]:02x}" if raw else "empty"
        raise AssetError(f"unsupported MPQ compression {codec}; only zlib (0x02) is supported")
    decoder = zlib.decompressobj()
    try:
        result = decoder.decompress(raw[1:], expected + 1)
    except zlib.error as error:
        raise AssetError(f"invalid zlib sector: {error}") from error
    if len(result) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise AssetError("MPQ sector decompressed length or stream boundary mismatch")
    return result


class Mpq:
    def __init__(self, path: Path):
        self.path = path
        self.size = path.stat().st_size
        header = self._read(0, 32)
        if header[:4] != b"MPQ\x1a":
            raise AssetError(f"{path}: expected an MPQ header at offset zero")
        _, header_size, archive_size, version, shift, hpos, bpos, nhash, nblock = struct.unpack("<IIIHHIIII", header)
        if version not in (0, 1) or shift > 15:
            raise AssetError(f"{path}: unsupported MPQ version or sector size")
        if header_size < (44 if version == 1 else 32) or archive_size > self.size:
            raise AssetError(f"{path}: malformed MPQ header")
        if version == 1:
            high_block, hash_high, block_high = struct.unpack("<QHH", self._read(32, 12))
            if high_block or hash_high or block_high:
                raise AssetError(f"{path}: MPQ 64-bit extended offsets are unsupported")
        if not nhash or nhash * 16 > MAX_TABLE or nblock * 16 > MAX_TABLE:
            raise AssetError(f"{path}: invalid or excessive MPQ table size")
        self.hashes = decrypt_table(self._read(hpos, nhash * 16), storm_hash("(hash table)", 0x300))
        self.blocks = decrypt_table(self._read(bpos, nblock * 16), storm_hash("(block table)", 0x300))
        self.nhash, self.nblock, self.sector_size = nhash, nblock, 512 << shift

    def _read(self, offset: int, count: int) -> bytes:
        if offset < 0 or count < 0 or offset + count > self.size:
            raise AssetError(f"{self.path}: MPQ range exceeds archive")
        with self.path.open("rb") as stream:
            stream.seek(offset)
            result = stream.read(count)
        if len(result) != count:
            raise AssetError(f"{self.path}: short MPQ read")
        return result

    def read(self, name: str) -> bytes | None:
        wanted = (storm_hash(name, 0x100), storm_hash(name, 0x200))
        start = storm_hash(name, 0) % self.nhash
        candidates = []
        for step in range(self.nhash):
            a, b, locale, _, index = struct.unpack_from("<IIHHI", self.hashes, ((start + step) % self.nhash) * 16)
            if index == MASK:
                break
            if index != 0xFFFFFFFE and (a, b) == wanted:
                candidates.append((locale != 0, locale != 0x409, index))
        if not candidates:
            return None
        index = min(candidates)[2]
        if index >= self.nblock:
            raise AssetError(f"{self.path}: invalid MPQ block index")
        pos, compressed, size, flags = struct.unpack_from("<4I", self.blocks, index * 16)
        if not flags & 0x80000000 or flags & 0x2000000:
            raise AssetError(f"{name}: MPQ deletion marker masks this asset")
        if flags & (0x10000 | 0x20000 | 0x100000 | 0x100):
            raise AssetError(f"{name}: encrypted, key-adjusted, delta or PKWARE member unsupported")
        if size > MAX_MEMBER or compressed > MAX_MEMBER + 65536:
            raise AssetError(f"{name}: excessive asset size")
        raw = self._read(pos, compressed)
        if not flags & 0x200:
            if len(raw) != size:
                raise AssetError(f"{name}: uncompressed size mismatch")
            return raw
        if flags & 0x1000000:
            return _inflate(raw, size)
        count = (size + self.sector_size - 1) // self.sector_size
        table_size = (count + 1) * 4
        if len(raw) < table_size:
            raise AssetError(f"{name}: truncated MPQ sector offsets")
        offsets = struct.unpack_from(f"<{count + 1}I", raw)
        if offsets[0] < table_size or offsets[-1] > len(raw) or any(a > b for a, b in zip(offsets, offsets[1:])):
            raise AssetError(f"{name}: malformed MPQ sector offsets")
        return b"".join(_inflate(raw[offsets[i]:offsets[i + 1]], min(self.sector_size, size - i * self.sector_size)) for i in range(count))


def safe_member(name: str) -> str:
    path = PureWindowsPath(name)
    if path.is_absolute() or path.drive or not path.parts or any(part in (".", "..") for part in path.parts):
        raise AssetError(f"unsafe asset path: {name!r}")
    if any(":" in part or "\x00" in part for part in path.parts):
        raise AssetError(f"unsafe asset path: {name!r}")
    normalized = "\\".join(path.parts)
    if not normalized.lower().startswith("creature\\crab\\") or not normalized.lower().endswith((".m2", ".blp")):
        raise AssetError(f"unexpected crab model dependency: {name!r}")
    return normalized


def texture_dependencies(model: bytes) -> list[str]:
    if len(model) < 0x64 or model[:4] != b"MD20" or struct.unpack_from("<I", model, 4)[0] not in (256, 257):
        raise AssetError("Crab.m2 must be a Vanilla MD20 version 256/257 model; modern models are incompatible")
    count, offset = struct.unpack_from("<II", model, 0x5C)
    if count > 64 or offset + count * 16 > len(model):
        raise AssetError("invalid M2 texture table")
    result = []
    for i in range(count):
        kind, _, length, position = struct.unpack_from("<4I", model, offset + i * 16)
        if kind != 0:
            continue  # Monster texture slots are supplied by the four CDI skins.
        if not length or position + length > len(model):
            raise AssetError("invalid embedded M2 texture name")
        try:
            name = model[position:position + length].rstrip(b"\0").decode("ascii")
        except UnicodeDecodeError as error:
            raise AssetError("non-ASCII M2 texture name") from error
        result.append(safe_member(name))
    return result


def prepare(archives: list[Path], output: Path) -> dict:
    sources = [Mpq(path) for path in archives]
    if not sources:
        raise AssetError("no MPQ archives found")
    extracted: dict[str, tuple[bytes, Path]] = {}

    def acquire(name: str) -> bytes:
        name = safe_member(name)
        for source in reversed(sources):
            raw = source.read(name)
            if raw is not None:
                extracted[name] = (raw, source.path)
                return raw
        raise AssetError(f"missing {name}; supply full, locally owned Vanilla 1.12 game data. The Azeroth Survivors trimmed Data folder does not contain the crab mesh/skins")

    model = acquire(CRAB_MODEL)
    for name in [*(rf"Creature\Crab\CrabSkin{skin}.blp" for skin in SKINS), *texture_dependencies(model)]:
        raw = acquire(name)
        if raw[:4] not in (b"BLP1", b"BLP2"):
            raise AssetError(f"{name}: expected a BLP texture")
    # Resolve every dependency before writing anything, and refuse symlink escapes.
    output = output.absolute()
    for component in (output, *output.parents):
        if component.is_symlink():
            raise AssetError(f"symlink output refused: {component}")
    manifest = {"schema_version": 1, "model_format": "Vanilla MD20 256/257", "files": [
        {"path": name.replace("\\", "/"), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(), "source_mpq": str(source.absolute())}
        for name, (raw, source) in extracted.items()
    ]}
    archive = make_mpq({name: raw for name, (raw, _) in extracted.items()})
    if output.exists() and output.read_bytes() != archive:
        raise AssetError(f"refusing to overwrite different archive: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(archive)
    # Independent readback checks file naming, hash tables and every byte.
    check = Mpq(output)
    for name, (raw, _) in extracted.items():
        if check.read(name) != raw:
            raise AssetError(f"generated archive verification failed: {name}")
    manifest["archive_sha256"] = hashlib.sha256(archive).hexdigest()
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--data-dir", type=Path, help="locally owned full Vanilla 1.12 Data folder")
    source.add_argument("--mpq", type=Path, action="append", help="source MPQ; repeat in low-to-high precedence order")
    parser.add_argument("--output", type=Path, default=Path("patch-Z.MPQ"), help="new archive path; never installs it into the game automatically")
    args = parser.parse_args()
    try:
        archives = args.mpq
        if args.data_dir:
            # Base archives first, then patch.MPQ, then numbered/letter patches.
            # All supported Vanilla source folders use at most one suffix char.
            def precedence(path: Path):
                stem = path.stem.lower()
                return (stem.startswith("patch"), stem != "patch", stem)
            archives = sorted((p for p in args.data_dir.iterdir() if p.is_file() and p.suffix.lower() == ".mpq"), key=precedence)
        manifest = prepare(archives, args.output)
    except (OSError, ValueError, struct.error) as error:
        print(f"prepare_crab_assets: {error}", file=sys.stderr)
        return 1
    print(f"Prepared {len(manifest['files'])} local crab assets in {args.output.absolute()}")
    print(f"SHA-256 {manifest['archive_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
