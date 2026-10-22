# Spiderless Azeroth Survivors

Windows x64 accessibility mod that replaces spider-like creature appearances
with Vanilla crabs. **Install one file: `avrt.dll` beside `AzerothSurvivors.exe`,
then start the game normally.**

The crab model, four skins and reflection texture are embedded in the DLL.
You do not need another WoW client, an asset preparation step, a separate MPQ,
a launcher or a DLL list. The mod does not download assets or extract them onto
disk at runtime. Existing game files in the adjacent `Data` directory are used
for the rest of the game as usual.

**Status: this single-file revision is awaiting its first Windows build and
in-game validation. There is no verified playable release yet.**

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
must be writable. Initialization or compatibility failures stop startup with
an error. Early DLL attach failures may produce Windows' DLL initialization
error before the mod can create its log.

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

The model, extra appearance and all skin slots change together. IDs, size
multipliers, alpha, sounds and other metadata stay intact. Model geometry may
affect derived collision/attachment behavior; this requires runtime validation.
Names, icons, web effects, world scenery and direct model loads bypassing the
creature DBC are outside this appearance change.

## Game updates

There is **no EXE hash or version lock**. Hook discovery uses unique masked
instruction signatures and checks function boundaries, prologues, native byte
allocation and DBC structure. Model IDs are derived from the current catalog.
An absent, ambiguous or incompatible native boundary stops startup.

Updates preserving these native contracts may remain compatible; arbitrary
future versions are not guaranteed. See [native evidence](docs/native-hook.md).
The reference hashes there identify research inputs only.

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
cmake --install build --config Release --prefix package
```

The standard-library Python checks run without compiling or launching a game:

```powershell
py -3 -B -m unittest discover -s tools -p "test_*.py" -v
```

Before calling a binary playable, validate automatic loading, AVRT forwarding,
embedded mesh/skin reads, animation, giant/boss scaling and affected zones on
Windows. This revision has not been compiled, loaded or visually tested yet.
