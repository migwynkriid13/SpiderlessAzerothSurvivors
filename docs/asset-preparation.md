# Embedded crab assets

Users install only `avrt.dll` beside `AzerothSurvivors.exe`. The DLL contains
the Vanilla crab model, four skins and reflection texture as RCDATA resources.
There is no asset preparation step, external asset folder, extra MPQ or runtime
download. It does not write embedded assets to disk.

The installed game MPQs contain the crab DBC definitions but omit the mesh and
four skins. Independent direct MPQ hash lookups, listfile checks and a bounded
scan of the EXE's embedded model candidates confirmed the expected files were
unavailable from the inspected 0.110.0 install. Reading the adjacent EXE/Data
alone cannot supply these missing files. The bundled inputs fill that gap.

`assets/crab/manifest.json` records the exact source files and hashes;
`assets/crab/NOTICE.md` records provenance and attribution. These files are
included in source so Windows builds need no other client or network download.
CMake configures `resources/crab_assets.rc.in` with their build-time paths and
the Windows resource compiler puts the bytes directly into the DLL.

`InitializeEmbeddedAssets` locates resources through an address in this DLL,
checks model/texture headers, validates the model's texture references against
the bundle and requires a Monster1 skin slot. Only then does it publish the
immutable lookup table. Archive paths compare without ASCII case and treat
forward/backslashes alike. Every unrelated asset remains with the game's
ordinary adjacent Data chain.

Both native existence checks and successful read results must recognize the
same embedded paths. The read hook returns a separate copy allocated through
the game's discovered byte allocator, so native callers can own and free it
normally. Resource memory remains read-only in the pinned DLL. It is never
returned as a Rust-owned Vec and is never freed by the game. The exact native
contracts and compatibility checks are described in `native-hook.md`.

The old `tools/prepare_crab_assets.py` is retained as an optional developer MPQ
inspection/extraction utility and as the parser used by the binary-input tests.
It is not installed in release packages and is not part of the user workflow.
The mod does not create or mount its output.

Resource validation and source tests do not prove rendered behavior. The
single-file revision still needs its first Windows build and in-game checks.
