# Working instructions

- Make changes on a feature branch and deliver them through a pull request.
  Do not merge or push implementation changes directly to `main` unless asked.
- Build CI runs only on pushes to `main`. Do not add PR or manual build triggers
  without a user request. The initial implementation is reviewed before its
  first build; report unrun compilation and runtime validation accurately.
- Do not gate the mod on a particular EXE hash or version. Game updates are
  frequent. Discover compatible hook signatures and validate native structure;
  stop safely when compatibility cannot be established.
- This repository is public. Do not commit game executables, DLL outputs,
  MPQs, extracted game assets, credentials or machine-specific analysis files.
- Keep analysis and temporary output inside ignored `runtime/`. The installed
  game and reconstruction repository are reference inputs; do not modify them
  as part of this project without a separate request.
- Explain findings, completed steps and blockers promptly in chat. During
  ongoing work, provide an update at least every 60 seconds.
- Use focused commits with `feature:`, `fix:`, `chore:` or `docs:` prefixes.
