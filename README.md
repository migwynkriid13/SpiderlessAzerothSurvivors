# Spiderless Azeroth Survivors

Windows x64 accessibility mod that replaces spider and spider-like creature
appearances with randomly selected Vanilla crab colors. Built for the original
Azeroth Survivors Windows game; this is a DLL mod, not a rebuilt game or Lua addon.

**Status: initial source implementation, awaiting its first Windows build and
in-game validation.** There is no verified playable release yet.

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

## Install and run after a build

1. Extract the build package. Place `SpiderlessLauncher.exe` and
   `SpiderlessAzerothSurvivors.dll` beside `AzerothSurvivors.exe`.
2. Install the prepared `Data/patch-Z.MPQ` as described above.
3. Start `SpiderlessLauncher.exe` each time you want the replacement enabled.

Alternatively, keep the launcher and DLL together elsewhere and supply the game:

```powershell
.\SpiderlessLauncher.exe "D:\AzerothSurvivors\AzerothSurvivors.exe"
```

The launcher starts a new game process, waits for Windows initialization, pauses
at the game entrypoint, loads and initializes the DLL, then resumes the game.
It stops its own new child if initialization fails. Use this launcher instead of
adding the DLL to `dlls.txt`; the DLL deliberately does no work in `DllMain`.
Administrator rights and attaching to an already running game are unnecessary.
Additional game command-line arguments are not currently supported.

`SpiderlessAzerothSurvivors.log` is created beside the game EXE, so that directory
must be writable. On the first creature catalog read, the mod checks the crab
mesh and four skins through the game's actual MPQ chain before remapping rows.
Missing assets or incompatible tables stop the game with an error.

Remove the launcher, mod DLL and this mod's `patch-Z.MPQ` to uninstall. The mod
does not edit the original EXE, game saves or original MPQs.

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

Development happens in pull requests. GitHub Actions builds the DLL and launcher
**only on pushes to `main`**, including merged PRs. PR pushes and PR creation do
not start a build; there is no manual workflow trigger. This lets the initial
implementation be reviewed before the first build.

After a main build, download the Windows x64 artifact from the repository's
Actions page. It contains the DLL, launcher, instructions, asset tool, source
commit/build link and SHA-256 checksums. No crab assets are included.

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

Before calling a binary playable, validate injection, all four crab skins,
animations, giant/boss scaling and the affected zones on Windows. No compilation,
DLL injection, game launch or visual validation was performed for this initial PR.
