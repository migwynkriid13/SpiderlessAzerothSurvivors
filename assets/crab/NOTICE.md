# Crab asset provenance

These six files are the original World of Warcraft Vanilla crab model and its
textures, made by Blizzard Entertainment. They are game artwork, separate from
this repository's source-code license; this project does not claim ownership
or grant a license to Blizzard's assets.

The files were obtained on 2026-10-22 from the Stonetavern Classic 1.8 mirror of
World of Warcraft 1.12.1, build 5875. The mirror's
[client page](https://stonetavern.app/clients/) identifies the client, and its
[file manifest](https://downloads.stonetavern.app/clients/classic-1.12.1/all/1.8/files.json)
identifies the source archives. `manifest.json` records exact archive URLs,
publisher-declared archive hashes, and independently calculated hashes and
sizes of every bundled file. The source archives were read by HTTP ranges;
their complete archive hashes were not independently checked.

`Crab.m2` is an unmodified `MD20` version 256 model. It has embedded skin
profiles and three texture entries: a fixed Bronze texture for a small
submesh, a native Monster1 texture slot (type 11) for the crab body, and the
reflection texture. The Monster1 slot permits the original Bronze, Ivory,
Saphire and Vermillian creature-display skins without modifying the mesh.
All five textures are original `BLP2` files with their mipmaps intact.

The DLL embeds only these six files. No game executable, complete MPQ archive
or downloaded launcher is included.
