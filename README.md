# Spiderless Azeroth Survivors

Windows x64 accessibility mod that replaces spider and spider-like creature
appearances with randomly selected Vanilla crab colors. Built for the original
Azeroth Survivors Windows game; this is a DLL mod, not a rebuilt game or Lua addon.

**Status: the automatic-loader revision is awaiting its first Windows build and
in-game validation.** The earlier launcher-based implementation compiled in
[the main workflow](https://github.com/migwynkriid13/SpiderlessAzerothSurvivors/actions/runs/39852896854);
that does not validate this new loading path or establish a playable release.

## What it changes

The DLL remaps creature appearance records by their model paths. It includes
spiders, tarantulas, crypt fiends, crypt lords and mechanical spider tanks, across
ordinary enemies, bosses and any other actors using those appearances. Current
0.110.0 data contains 120 matching display records across 13 model paths.
[The coverage inventory](docs/spider-coverage.json) records the inspected zones
and source data.

Vanilla has one crab mesh with four skins: Bronze, Ivory, Saphire and Vermillian.
Each original appearance gets a random skin when the game starts and keeps it
throughout that session. Creatures sharing the same original appearance share a
color; there is no separate random roll per spawned creature.

Display IDs, size multipliers, alpha and non-appearance metadata stay intact.
The model and all skin slots change together. Model geometry can affect derived
collision/attachment behavior; this still requires game testing. Names, sounds,
icons, web effects and world scenery are outside this creature-model change.
Direct model loads that bypass the creature DBC are outside this hook's coverage.

## Required crab assets

The supplied Azeroth Survivors MPQs list crab appearances but omit the crab mesh
and four skins. You need locally owned, compatible **Vanilla 1.12 game data** to
prepare those files. Nothing in this repository downloads or ships game assets.

With Python 3.10 or newer, run:

```powershell
py -3 tools/prepare_crab_assets.py --data-dir "D:\WoW-1.12\Data" --output patch-Z.MPQ
```

Copy the resulting `patch-Z.MPQ` into Azeroth Survivors' `Data` folder. The tool
refuses to overwrite a different existing output. See
[asset preparation](docs/asset-preparation.md) for archive options and supported
formats. Extraction against real crab assets remains unverified because those
assets were absent from the available local data.

## Install and run after a release

1. Download **`avrt.dll`** from the repository's
   [GitHub Releases](https://github.com/migwynkriid13/SpiderlessAzerothSurvivors/releases).
2. Put it beside `AzerothSurvivors.exe`. Keep its exact filename `avrt.dll`.
3. Install the prepared `Data/patch-Z.MPQ` described above.
4. Start **`AzerothSurvivors.exe` normally**.

No separate launcher, DLL list, injector, EXE modification or administrator
rights are needed. The DLL uses the Windows AVRT library name already imported
by the game, forwarding the three required AVRT functions to the real system
library. Windows normally searches the EXE directory before System32 for this
library. Custom DLL search policies, Wine overrides or another mod using
`avrt.dll` can affect loading; do not overwrite a different existing `avrt.dll`.
There is no mod-chaining support in this version.

During load, the proxy arms a temporary breakpoint at the EXE's entrypoint.
After Windows finishes DLL initialization, a minimal exception handler redirects
execution to a normal bootstrap function. That function restores the entrypoint,
loads the real system AVRT library, initializes the spider replacement, then
restores the game's original CPU context. Initialization runs before the game
entrypoint and outside both `DllMain` and the exception handler.

`SpiderlessAzerothSurvivors.log` is created beside the game EXE, so that directory
must be writable. On the first creature catalog read, the mod checks the crab
mesh and four skins through the actual MPQ chain before remapping rows. Missing
assets or incompatible tables stop the game with an error. Startup failures
that occur before the bootstrap runs may produce Windows' DLL initialization
error instead; no log is guaranteed in that early failure case.

Remove this mod's `avrt.dll` and `patch-Z.MPQ` to uninstall. Do not place the
proxy in Windows' system directories. The mod changes process memory only; it
does not edit the original EXE, saves or original MPQs. The crab overlay remains
required: a DLL filename alone cannot supply assets absent from the game data.

## Game updates

There is **no EXE hash or version lock**. The mod searches executable sections
for a unique masked asset-reader signature and checks the function boundary,
prologue and live DBC structure. Model IDs are derived from the current model
catalog instead of a fixed mob list.

Updates that preserve this function shape and ABI can work without changing the
mod. An update that changes either may require a new signature or implementation;
future compatibility has not been established. An absent or ambiguous signature
stops startup. [Native hook evidence](docs/native-hook.md) records the inspected
ABI and its limits. Reference hashes there identify research inputs only.

## Build and contribute

Development happens in pull requests. GitHub Actions builds the DLL and
publishes a GitHub Release
**only on pushes to `main`**, including merged PRs. PR pushes and PR creation do
not start a build; there is no manual workflow trigger. Each revision can be
reviewed before its main-branch build.

After a successful main build, its GitHub Release contains standalone
`avrt.dll`, a ZIP with the DLL/instructions/asset tools, and SHA-256 checksums.
Each release points to its exact source commit and build. The same files remain
available as an Actions artifact. No crab assets are included.

For a deliberate local build on Windows, install Visual Studio 2022 or newer
with Desktop development with C++, its Windows SDK, and CMake 3.25 or newer:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package
```

No game files or external libraries are required to compile. Asset-tool tests
run without compilation or game data:

```powershell
py -3 -B -m unittest discover -s tools -p "test_*.py" -v
```

Before calling a binary playable, validate automatic loading and AVRT
forwarding, all four crab skins, animations, giant/boss scaling and the affected
zones on Windows. The automatic-loader revision has not been compiled or run
yet. No game launch or visual validation has been performed. The successful
earlier launcher build does not test this proxy, breakpoint bootstrap or AVRT
forwarding.
