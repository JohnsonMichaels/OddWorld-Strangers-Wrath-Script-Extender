# SWSE Roadmap

> **Retired 2026-09-28.** This was the project's first roadmap, written in
> July 2026 before 1.0 shipped, and it is no longer maintained. The
> byte-identical repacker, the first mod to load in game, the mod system and
> the Mod Loader all shipped in 1.0. The page is kept so that old links still
> land somewhere.

Where its topics live now:

- The plan after 1.1: `swse/research/SWSE2_ROADMAP.md`. The renderer:
  `swse/research/GRAPHICS_ROADMAP.md`.
- Factions and town raids: `swse/research/FACTIONS.md`. This page said the
  engine never creates NPCs at runtime; it does, from gibs and to refill
  spawn pools.
- The first-person weapon shaded as the ground behind it:
  `swse/research/GRAPHICS_RTGI.md`.
- HD alpha cutouts: "Transparent textures - the trap" in
  [SWSE_FEATURES.md](SWSE_FEATURES.md). The packing pipeline still drops
  1-bit alpha, so `tools/alpha_check.py --quarantine` keeps those textures
  vanilla.
- What SWSE does today: [SWSE_FEATURES.md](SWSE_FEATURES.md). What changed:
  [CHANGELOG.md](CHANGELOG.md).

The old text is in the git history, for example at tag `v1.0.2`.
