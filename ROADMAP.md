# SWSE Roadmap

> **Retired 2026-09-28.** This was the project's first roadmap, written in
> July 2026 before 1.0 shipped, and it is no longer maintained. The
> byte-identical repacker, the first mod to load in game, the mod system and
> the Mod Loader all shipped in 1.0. The page is kept so that old links still
> land somewhere.

Where its topics live now:

- The plans after 1.1 are kept in the development repository's research
  notes; nothing on them is part of 1.1.1.
- NPC-vs-NPC fights and town raids: not supported in 1.1.1. This page said
  the engine never creates NPCs at runtime; it does: gib spawns, and
  spawners releasing the bodies they pool.
- The first-person weapon shaded as the ground behind it: a graphics
  research note in the development repository.
- HD alpha cutouts: "Transparent textures - the trap" in
  [SWSE_FEATURES.md](SWSE_FEATURES.md). The packing pipeline still drops
  1-bit alpha, so the HD build keeps those textures vanilla.
- What SWSE does today: [SWSE_FEATURES.md](SWSE_FEATURES.md). What changed:
  [CHANGELOG.md](CHANGELOG.md).

The old text is in the git history, for example at tag `v1.0.2`.
