# Working instructions

- Make changes on a feature branch and deliver them through a pull request.
  Do not merge or push implementation changes directly to `main` unless asked.
- Build/release CI runs only on pushes to `main`. Do not add PR or manual build
  triggers without a user request. Deliver source changes in a PR before the
  main build; report compilation and runtime validation accurately.
- The install is app-local `avrt.dll` beside `AzerothSurvivors.exe`, with normal
  EXE startup. Do not reintroduce a separate launcher or require a DLL list.
  Publish the standalone DLL, ZIP and checksums through GitHub Releases.
- Embed the requested crab mesh, skins and their texture dependencies in the
  DLL. Users must not prepare/install a separate MPQ, supply a WoW client or
  download model files at runtime. Read the game's adjacent `Data` as usual
  for everything outside this small embedded replacement set.
- Do not gate the mod on a particular EXE hash or version. Game updates are
  frequent. Discover compatible hook signatures and validate native structure;
  stop safely when compatibility cannot be established.
- This repository is public. The specifically requested crab mesh, four skins
  and embedded texture dependencies are authorized asset inputs. Do not commit
  other game assets, game executables, DLL outputs, full MPQs, credentials or
  machine-specific analysis files. Record the crab inputs' provenance.
- Keep analysis and temporary output inside ignored `runtime/`. The installed
  game and reconstruction repository are reference inputs; do not modify them
  as part of this project without a separate request.
- Explain findings, completed steps and blockers promptly in chat. During
  ongoing work, provide an update at least every 60 seconds.
- Use focused commits with `feature:`, `fix:`, `chore:` or `docs:` prefixes.
