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
and word two must not be inspected. The DLL leaves native errors untouched for reads outside the embedded asset set.
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
through a table or indirect dispatch. It modifies successful CreatureDisplayInfo
reads and serves the small embedded crab resource allowlist before calling the
original reader. Every other read is forwarded unchanged.

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

The availability entry also uses a 15-byte trampoline. Its complete displaced
instructions are `push r14; push rsi; push rdi; push rbx; sub rsp,0x28;
mov rsi,r8; mov rdi,rdx`, bytes `41565657534883ec284c89c64889d7`.
They contain no relative branch or RIP-relative operand. Both original forwarding
trampolines are prepared and all signatures/ownership contracts checked before
either native entry is modified.

Install before the game entrypoint executes. The app-local AVRT proxy runs its
bootstrap after Windows DLL initialization and completes hook installation
before restoring the original entrypoint context. There is no scanning thread
or separate launcher. Keep the DLL and trampoline loaded for process lifetime.

## Automatic app-local loading

The current reference EXE directly imports `avrt.dll` with these three names:

| Import | Current IAT RVA |
| --- | --- |
| `AvSetMmThreadPriority` | `0x5da55c8` |
| `AvRevertMmThreadCharacteristics` | `0x5da55d0` |
| `AvSetMmThreadCharacteristicsW` | `0x5da55d8` |

It does not directly import `version.dll` or `winmm.dll`, and has no delay-import
directory. The build therefore produces **`avrt.dll`**, forwarding those three
SDK-typed exports to the real AVRT library loaded by absolute System32 path.
The loaded real module must differ from the proxy to prevent redirection
recursion. Before forwarding is ready, wrappers return documented failure
values with `ERROR_NOT_READY`; they never load libraries from another DLL's
initialization routine.

Windows' standard unpackaged-app search order looks in the EXE directory before
System32 after redirection, loaded-module and KnownDLL processing. The design
relies on this normal app-local resolution; it does not change registry entries,
DLL search policy, Wine overrides, the installed game or its import table on
disk. Existing `avrt.dll` mods cannot be chained by this implementation.

`DllMain` validates the mapped AMD64 image, derives the entrypoint from its PE
header, records its original byte and protection, registers a vectored exception
handler, and arms an `INT3` at that entrypoint. It does not call LoadLibrary,
wait on a worker, initialize the mod, or show a User32 error. The current
reference entrypoint is RVA `0x5c04608`, with first byte `0x48`; those are
provenance values only. The EXE's current TLS callback list is empty. A changed
image with executable TLS callbacks must be rejected because game-owned code
could run before the entrypoint bootstrap.

The handler matches the expected entrypoint and startup thread. It accepts a
breakpoint context RIP at the entrypoint or immediately after the trap byte.
It only copies the supplied CPU context into preallocated state, keeps the
standard `CONTEXT_ALL` flags (the saved buffer has no variable XSTATE tail),
rewinds its saved RIP to the entrypoint and redirects the returning context
to a normal bootstrap routine. A normal Win64 incoming stack alignment is
required before redirecting to C++.
After the handler returns, bootstrap restores the original byte/protection and
instruction cache, removes the temporary handler, resolves system AVRT exports,
initializes the spider mod and restores the saved context with `RtlRestoreContext`.
Heavy work happens outside both the loader lock and exception handler. Startup
failures terminate this process; no arbitrary running process is attached.

This follows Microsoft's restrictions for
[DllMain](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices)
and [vectored exception handlers](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nc-winnt-pvectored_exception_handler),
using [RtlRestoreContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlrestorecontext)
for the final transfer. See also the documented
[DLL search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order).

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

## In-memory crab asset resolution

Only `avrt.dll` is installed beside the EXE. The crab mesh, skins and texture
dependencies are Win32 resources inside the DLL. Startup loads and validates
the complete resource set before enabling either hook. The DLL remains pinned
for process lifetime, so resource spans remain valid. No overlay MPQ, loose
asset extraction, network access or change to the adjacent `Data` is required.
The game continues resolving all other files from its own archives.

A read override alone is insufficient. The asynchronous MPQ asset-reader owner
`1422911b0..1422913c3` calls the native availability predicate at `142291259`,
tests AL at `14229125f`, and reaches the shared read at `142291276` only when
that predicate succeeds. The predicate is RVA `0x5aa1410`, declared unwind
extent `[145aa1410, 145aa1471)` (97 bytes). Its ABI is `RCX=chain`,
`RDX=UTF-8 path`, `R8=path byte count`, with a Boolean in AL. Its original body
walks archives from highest priority and checks entry existence/delete markers.
It never consults the cached byte map used by the first callee of `Chain::read`.

The DLL therefore hooks **both** availability and read. Both use the same exact,
case-insensitive, slash-normalized resource lookup. Availability returns true
only for embedded paths, forwarding every other query to the original. Read
serves those bytes directly before consulting archives. This covers the
asynchronous existence precheck and direct synchronous consumers through the
shared chain reader. It does not fabricate success for missing unrelated files.

The model URL construction owners `142294290..14229467a` and
`145119e90..14511a2fb` lowercase model paths and recognize `.mdx` / `.mdl`
suffixes before constructing the `.m2` archive path. The suffix checks are
visible at `14229432a` / `142294335` and `145119f2f` / `145119f3a`.
`Creature\Crab\Crab.mdx` in CreatureModelData therefore needs the embedded
entry `Creature\Crab\Crab.m2`, together with the selected skins and any
additional texture paths referenced by the mesh.

## Native allocation ownership

A resource pointer cannot be returned as a Rust Vec: the game's consumers free
successful read buffers. Each embedded read instead allocates a fresh native
byte vector with the game's verified allocator, copies the immutable resource
bytes, and returns `{capacity=length, data=allocation, size=length}`. Ownership
passes to the game exactly as it does for an original successful read. The DLL
never hands out static resource storage, its own CRT memory, or a guessed heap
allocation. Allocation failure stops the process without falling back to a
spider model.

The reference global allocation entry is `145baedc0..145baee1d` (93 bytes),
with size in RCX, alignment in RDX and the allocated pointer in RAX. The matching
deallocation entry is `145baee20..145baee53` (51 bytes). Their full instruction
signatures are scanned independently and must each be unique, at a registered
native function boundary. Relative external displacements are masked.

Discovery also proves these functions are connected to the **active asset read
path**, rather than merely finding an unused allocator. Follow the first CALL
in the matched reader (instruction offset 53) to cached-byte reader
`145aa3940`. Within that function's declared unwind extent, the unique byte
clone signature at `145aa3af3` sets alignment to 1 and calls `1400021c0` at
`145aa3afb`. That thunk jumps to the discovered allocator. The unique byte-drop
signature at `145aa3b62` sets alignment to 1 and calls `1400021d0` at
`145aa3b6c`; its thunk jumps to the discovered deallocator. Runtime follows
these call edges and allows either a direct call or one native JMP thunk.

Both allocator branches must lead to one compatible heap helper, reference
`145bc6200..145bc623c` (60 bytes). Its masked body is validated and its native
boundary checked. Its indirect calls must resolve to the actual system
`GetProcessHeap` and `HeapAlloc` exports. The matched deallocator must resolve
to the same system `GetProcessHeap` and `HeapFree` exports. These checks verify
the current alignment-1 ownership contract before a buffer reaches the game.
If the game changes allocator, native ABI, function shape, or call ownership,
startup stops safely and needs updated compatibility evidence.

All addresses in this section are provenance coordinates only. Runtime computes
call targets and IAT addresses from the matched instructions; it does not
select functions by these RVAs or compare an EXE hash/version.

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

The first display read obtains CreatureModelData through the original reader.
This one native-owned buffer is retained for process lifetime, with an 8 MiB
capacity budget. Further display reads reuse it. Crab assets have already been
validated as DLL resources before hook installation; this metadata probe never
bypasses the embedded overlay to look for them in the installed archives.
DBC/schema or embedded-asset failures stop the process before a modified display
table reaches its caller.

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

An independent full `.text` scan of the reference EXE found exactly one match
each for the availability, allocation, deallocation and heap-helper signatures;
the existing reader signature remains unique. Static call-edge checks confirmed
the cached-byte clone/drop targets and the three native heap import names.
These inspections did not compile or execute the DLL.

The earlier launcher-based implementation compiled successfully in the
[main build](https://github.com/migwynkriid13/SpiderlessAzerothSurvivors/actions/runs/39852896854).
The AVRT proxy, automatic bootstrap and embedded-resource hooks have not been
compiled or executed in this source update. No game launch, rendered crab
validation or Windows runtime test has been performed. Startup validates the
bundled assets and observed native contracts, but static evidence does not
establish live visual parity.
