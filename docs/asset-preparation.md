# Preparing local crab assets

The researched Azeroth Survivors 0.110.0 `Data` folder contains the Vanilla
crab DBC definitions, but omits the crab mesh and its four skins. The DLL needs
the asset overlay as well as the display remap. No original game assets are
included in this repository or its source builds.

Use a full, locally owned **Vanilla WoW 1.12** `Data` folder as the source:

```powershell
py -3 tools/prepare_crab_assets.py --data-dir "C:\Games\World of Warcraft\Data" --output patch-Z.MPQ
```

Alternatively, pass the required source archives explicitly. Repeated `--mpq`
arguments are ordered from lowest to highest precedence:

```powershell
py -3 tools/prepare_crab_assets.py --mpq "C:\Games\WoW\Data\model.MPQ" --mpq "C:\Games\WoW\Data\texture.MPQ" --output patch-Z.MPQ
```

The tool resolves `Creature\Crab\Crab.m2`, the Bronze, Ivory, Saphire and
Vermillian skins, and each embedded file texture referenced by the model. The
spelling `Saphire` is the original asset name. It accepts Vanilla `MD20` versions
256/257; modern Retail crab models use an incompatible format.

The output is a normal stored MPQ overlay with encrypted directory tables and
a listfile. Every extracted asset is read back and compared with its source
bytes. The tool reports the archive SHA-256 and refuses to replace a different
existing output. It does not install the archive or change the game. When ready
to install, copy the resulting `patch-Z.MPQ` into Azeroth Survivors' `Data`
folder, and follow the DLL installation instructions in the README. Check that
the game does not already use that archive name before copying.

Python 3 and its standard library are sufficient. Source archives must be
ordinary MPQ v0/v1 files with raw or zlib-compressed members. Encrypted members,
PKWARE and other compression codecs, delta patches and extended 64-bit offsets
are rejected explicitly. No files are written if a required crab asset is
missing.

The six parser/extractor tests use synthetic meshes and textures, including
compressed sectors and complete overlay extraction:

```powershell
py -3 -B -m unittest discover -s tools -p test_prepare_crab_assets.py -v
```

The installed `CreatureModelData.dbc`, `CreatureDisplayInfo.dbc` and a real
`MineSpider.m2` were also read successfully with this parser. Preparing real
crab assets remains unverified because a complete local Vanilla asset source
was unavailable during development.
