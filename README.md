# Spiderless Azeroth Survivors

Windows x64 accessibility mod that replaces spider-like creature appearances
with Vanilla crabs. **Install one file: `avrt.dll` beside `AzerothSurvivors.exe`,
then start the game normally.**

The crab model, four skins and reflection texture are embedded in the DLL.
You do not need another WoW client, an asset preparation step, a separate MPQ,
a launcher or a DLL list. The mod does not download assets or extract them onto
disk at runtime. Existing game files in the adjacent `Data` directory are used
for the rest of the game as usual.

The single-file release has built successfully, and crab rendering has been
confirmed by a player using WoWSilicon on macOS. The new size changes below are
available in releases. The 0.112.0 compatibility and scaling-fallback update
below is awaiting its Windows build and in-game validation.

## Install

1. Download **`avrt.dll`** from
   [GitHub Releases](https://github.com/migwynkriid13/SpiderlessAzerothSurvivors/releases).
2. Put it beside **`AzerothSurvivors.exe`**, keeping the filename `avrt.dll`.
3. Start **`AzerothSurvivors.exe` normally**.

The game already imports the Windows AVRT library. This app-local proxy loads
through that import and forwards the three required functions to the real
system library. It initializes the mod before the game entrypoint, outside
Windows DLL initialization and exception dispatch. No EXE modification on disk,
registry change, administrator rights or separate process is required.

Custom DLL search policies, Wine overrides or another mod named `avrt.dll` can
affect loading. Do not overwrite a different existing `avrt.dll`; mod chaining
is not supported. Do not put this DLL in Windows system directories.

The mod writes `SpiderlessAzerothSurvivors.log` beside the game, so that folder
must be writable. Failures in automatic loading, embedded assets or required
asset hooks stop startup with an error. If only the optional scaling hooks are
incompatible, the log reports that size multipliers are disabled and crab
replacement continues at the game's original sizes. Early DLL attach failures
may produce Windows' DLL initialization error before the mod can create its log.

Delete this mod's `avrt.dll` to uninstall. Original EXE, saves and MPQs are not
changed. If upgrading from the earlier source-only overlay design, its manually
installed `patch-Z.MPQ` is no longer needed; the mod never creates an MPQ itself.

## Replacement coverage

The appearance policy identifies model paths containing `spider`, `tarantula`,
`cryptfiend` or `cryptlord`, including mechanical spider tanks. The researched
0.110.0 data contains 120 matching display records across 13 model paths,
covering ordinary enemies and bosses in the
[coverage inventory](docs/spider-coverage.json).

Vanilla has one crab mesh with four colors: Bronze, Ivory, Saphire and
Vermillian. Each source appearance receives a random color at startup, stable
throughout the session. Creatures sharing an original appearance share a color;
there is no separate random roll per spawned creature.

The model, extra appearance and all skin slots change together. Original DBC
IDs, scale, alpha, sounds and other metadata stay intact.

Replaced creatures receive these visual size multipliers relative to their
existing size, including any size adjustment already applied by the game:

| Creature role | Size |
| --- | --- |
| Boss | 4× |
| Lair guardian | 4× |
| Ordinary mob, including ordinary elites | 2× |

The role comes from the live creature creation path. A boss and ordinary mob
can share an appearance without sharing a size multiplier. Creature names do
not determine the role: the ordinary elite named Guardian of Icecrown uses 2×.
Native appearance changes and restoration reuse the same actor role and apply
the multiplier once to the game's supplied scale. Other creature appearances
retain their original size.

Scaling uses the game's visual appearance publisher, leaving health, damage,
AI attack reach and the client bounding-radius/combat-reach fields at their
original values. The enlarged mesh and attachments still need visual testing.
Names, icons, web effects, world scenery and direct model loads bypassing the
creature DBC are outside this appearance change.

## Game updates

There is **no EXE hash or version lock**. Hook discovery uses unique masked
instruction signatures and checks function boundaries, prologues, creature
creation call sites, native byte allocation and DBC structure. Model IDs are
derived from the current catalog.

The scaling signatures ignore reviewed internal field/stack displacements that
the DLL does not use, while retaining instruction, argument and call-target
checks. If scaling cannot be established safely, its hooks are not installed;
the independently validated crab asset hooks remain enabled. Check the log to
see whether the 4×/2× multipliers are active. A failure while writing a hook, or
an incompatible required asset boundary, still stops startup.

Native hooks cannot guarantee compatibility with arbitrary game updates.
Changes to required calling conventions, allocator ownership or DBC formats
can still need a mod update. See [native evidence](docs/native-hook.md), including
the 0.112.0 compatibility fix. Reference hashes identify research inputs only.

## Build and releases

Development happens through pull requests. Only a push to `main`, including a
merged PR, builds and publishes a GitHub Release. PR pushes do not build, and
there is no manual trigger. Releases contain standalone `avrt.dll`, a ZIP with
documentation, and SHA-256 checksums tied to the exact source commit/build.

The six crab inputs are tracked and compiled as Windows RCDATA resources.
[Asset provenance](assets/crab/NOTICE.md) and the
[manifest](assets/crab/manifest.json) record their source and hashes.
[Embedded asset design](docs/asset-preparation.md) explains dependency checks
and native ownership. No external asset source is needed to build this revision.

For a deliberate local build, use Windows x64, Visual Studio 2022 or newer with
Desktop development with C++ and the Windows SDK, plus CMake 3.25 or newer:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix package
```

The standard-library Python checks run without compiling or launching a game:

```powershell
py -3 -B -m unittest discover -s tools -p "test_*.py" -v
```

The C++ regression target checks catalog-derived replacement IDs, preserved DBC
metadata, shared appearances with different live roles, 2×/4× multipliers and
appearance restoration. It runs in the main build before packaging; the test
executable is not installed or included in releases.

The existing release's model rendering is player-confirmed on WoWSilicon.
The 4×/2× revision compiled and passed its C++ regressions. The 0.112.0
compatibility and scaling-fallback changes have not been compiled or tested
in-game yet. Validate bosses, guardians, ordinary mobs, appearance restoration,
animation and affected zones after the next main build.
