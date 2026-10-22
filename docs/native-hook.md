# Native read hook: Azeroth Survivors 0.110.0

The reference examined is Windows x64, preferred image base `0x140000000`,
SHA-256 `e069bf3152f8d8f7160d66388907fc181c733bd170aa150d3b7ff7adf251e048`.
All addresses below were recovered from these exact installed bytes on
2026-10-22 and are provenance coordinates. The runtime does not watch or compare
the EXE hash, version or fixed addresses. It locates the read boundary with the
unique masked instruction signature in `include/target_profile.hpp`.

## Read boundary and ownership

The shared archive-chain read entry is RVA `0x5aa1310`, file offset `0x5aa0710`.
Its declared unwind extent is `[0x145aa1310, 0x145aa13cf)` (191 bytes), with
SHA-256 `dc03f2db1c3a5303950cd3ac1a5bb74afc8c103855ec6ff77d676c22d172ba97`.
Its source-level identity is inferred from its callers and archive fallback;
it is not an exported symbol.

The Win64 register contract is:

| Register | Value |
| --- | --- |
| RCX | Address of the caller's 24-byte `Result<Vec<u8>, Error>` output |
| RDX | Archive chain/context pointer |
| R8 | Borrowed UTF-8 internal path pointer |
| R9 | Path byte count; no terminating NUL is required |
| RAX on return | The original output address |

On success the output is three 64-bit words: allocation capacity, byte pointer,
byte length. On error word zero is `UINT64_MAX`, word one holds the error owner,
and word two must not be inspected. The DLL must leave errors untouched.
Assembly at `145aa1351..145aa135d` copies the three success words; the error
branch at `145aa1374..145aa137f` writes word one and the `-1` discriminator;
`145aa13c1` restores the output pointer into RAX.

Direct current call sites include:

| Read | Call site | Path address |
| --- | --- | --- |
| CreatureModelData for the creature appearance catalog | `145a97592` | `14726156d` |
| CreatureDisplayInfo for the creature appearance catalog | `145a97a40` | `1472615a1` |
| CreatureDisplayInfo for another DBC consumer | `145ab5630` | `147262dd8` |
| CreatureDisplayInfo for another DBC consumer | `145ae1816` | `147264139` |

The hook is at the shared entry, so it also handles callers that obtain a path
through a table or indirect dispatch. It changes only successful
CreatureDisplayInfo reads. Other reads are forwarded unchanged.

## Detour

The first 15 bytes are complete, position-independent instructions:

```text
55                push rbp
41 56             push r14
56                push rsi
57                push rdi
53                push rbx
48 83 ec 60       sub rsp, 0x60
48 8d 6c 24 60    lea rbp, [rsp+0x60]
```

Exact bytes: `5541565657534883ec60488d6c2460`.

A 14-byte RIP-indirect absolute jump (`FF 25 00 00 00 00` plus the 64-bit
hook address) and one NOP replace those 15 bytes. The trampoline contains the
original 15 bytes and an absolute jump to `image_base + 0x5aa131f`. None of the
copied instructions contains a relative branch or a RIP-relative data address,
so no instruction relocation is required. Flush the instruction cache after
writes and preserve/restore page protection.

Install before the main game thread runs. The launcher must synchronously
complete initialization and verify success before resuming its suspended child.
No scanning thread should race the catalog loader. Keep the DLL and trampoline
loaded for the lifetime of the process.

## Signature discovery and updates

The 96-byte entry signature masks only the two external `CALL rel32`
displacements (byte offsets 54..57 and 92..95). An independent scan over every
byte of the reference executable's complete 98,188,288-byte `.text` raw section
found exactly one match, RVA `0x5aa1310`. Installation requires exactly one match
inside executable image sections and the intact 15-byte displaced prologue.
No version or file-hash match is required.

This tolerates code movement and different call destinations across updates
while checking the observed function shape and result-handling instructions.
An update that changes the native ABI or this shape needs a new signature;
zero or multiple matches must stop startup clearly. Signature matching is
structural compatibility evidence, not a guarantee that an arbitrary future
EXE has an identical Rust ABI. Validate the live WDBC schema before mutation.

## MPQ asset resolution

Use a prepared `patch-Z.MPQ` containing the user-supplied vanilla crab mesh and
skins. DBC rows naming a model do not establish that its assets are installed.
The game can take an archive lookup path before the shared read boundary, so
a loose read override alone would not establish missing-asset availability.
A normal MPQ overlay lets all archive existence and read paths find the files.

The current chain-open owner is `145a9ff80..145aa0e01`. Its patch collector
`145b486b0..145b488de` calls predicate `145aa8e20..145aa9060` before including a
filename. That predicate lowercases its input, compares the first six bytes to
`patch-`, the final four bytes to `.mpq`, and counts the middle Unicode
characters. It accepts exactly one middle character. The decisive instruction
ranges are `145aa8fd2..145aa9004` (prefix/suffix) and
`145aa902e..145aa9052` (middle count equals one). Thus `patch-Z.MPQ` is accepted;
`patch-10.MPQ` is not. The caller sorts the patch collection before mounting,
with ordinary `patch.MPQ` first and `speech2.MPQ` after the numbered/lettered
patches. This does not claim a custom overlay beats an independently supplied
later-priority archive; asset preflight must use the actual live chain.

The model URL construction owners `142294290..14229467a` and
`145119e90..14511a2fb` lowercase model paths and recognize `.mdx` / `.mdl`
suffixes before constructing the `.m2` archive path. The suffix checks are
visible at `14229432a` / `142294335` and `145119f2f` / `145119f3a`.
`Creature\Crab\Crab.mdx` in CreatureModelData therefore needs the archive
entry `Creature\Crab\Crab.m2`, together with all selected `CrabSkin*.blp`
textures beside that model. No Blizzard mesh or texture is shipped in this
source repository.

## Replacement point

Modify the raw CreatureDisplayInfo WDBC buffer after a successful read and
before the caller parses/caches it. CreatureModelData field 2 maps model IDs
to names. CreatureDisplayInfo field 1 selects a model; fields 6, 7 and 8 are
texture names in the same file's string block. All row/field/string ranges must
be checked against the actual file size before access.

Classify the current CreatureModelData paths by the case-insensitive substrings
`spider`, `tarantula`, `cryptfiend` and `cryptlord`; derive the crab model ID from
its canonical path. No fixed display/model ID list or DBC hash gate is used.
Choose among four distinct crab skins from the same display buffer, snapshot
their complete records, and copy fields 1 (model), 3 (extra appearance) and
6/7/8 (all skin slots) into each classified row. Original display IDs, scale,
alpha, sound and other metadata remain intact. Never copy only the model ID:
that can bind spider skins to crab meshes. Never copy string offsets from a
different DBC buffer.

The first display read probes CreatureModelData, the crab mesh and its four
skins through the original reader. These six native-owned buffers are retained
for process lifetime, bounded by an 8 MiB total capacity budget, avoiding a
dependency on a version-specific Rust allocator/deallocator ABI. Further reads
reuse the metadata and probe result. The DLL does not synthesize a Rust Vec or
free native allocations with its own CRT. DBC/schema or asset failures stop the
process before the modified display table reaches its caller.

A process seed combined with the original display ID can select a repeatable
random crab variant for that display throughout the process. This varies
source display appearances; it does not claim a different random roll for
every individual creature sharing one display ID.

## Evidence limits

Static evidence came from the complete current declared-unwind instruction
inventory, independent `objdump` inspection and a scoped REA/Ghidra 12.1.4
raw import of the exact 191-byte read owner. Its Evidence ID is
`ev_cb10426b019473338436dfdf62cc81d5c9e75b114c45c4f06792153874565723`,
profile digest `fac186f902edc18b199b1c48aa999b8cfc91334046e8f96f572e99d5722679ea`.
The raw import lacks surrounding PE imports/data and uses Ghidra's generic GCC
compiler specification; register and allocation contracts above were checked
against the actual Win64 instructions and import table, not inferred from its
parameter labels.

No compilation, DLL injection, game launch, rendered crab validation or Windows
runtime test has been performed. The catalog's crab entries do not prove the
installed MPQs contain the corresponding mesh/textures. Asset availability
must be established separately before allowing gameplay to start.
