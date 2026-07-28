# SWSE Roadmap

The goal: a complete modding platform for Oddworld: Stranger's Wrath HD that lets the
community create, share, and install mods - no reverse-engineering knowledge required.

## Architecture (bottom to top)

```
+---------------------------------------------------------+
|  4. Mod Loader (GUI)                               |
|     Windowed editor: NPC stats, weapons, quests,        |
|     locations, textures. "New Mod" -> edit -> save.     |
+---------------------------------------------------------+
|  3. Mod system                                          |
|     MODS/ folder; each mod = manifest + patches.        |
|     Loader merges mods, rebuilds archives, backs up     |
|     originals, one-click enable/disable.                |
+---------------------------------------------------------+
|  2. bounty CLI                                          |
|     catch (extract) / cashin (repack) / diff / verify   |
+---------------------------------------------------------+
|  1. oddforge format library (Python)                    |
|     SMB container + TOC node graph + .smh scripts       |
|     parse AND write, byte-identical round-trip          |
+---------------------------------------------------------+
```

## Design principles

- **Mods are patches, not archives.** A mod folder contains *edits* (changed script
  text, replaced textures, changed values) plus a `mod.json` manifest - never full
  game archives. Keeps mods small, legally clean (no game assets distributed), and
  lets multiple mods coexist by merging patches before rebuild.
- **Originals are sacred.** The loader backs up any archive before touching it and
  can restore a vanilla install in one command.
- **Byte-identical or bust.** The repacker is only trusted once `catch` + `cashin`
  with zero edits reproduces the original file exactly, for all 1,222 archives.

## Milestones

| # | Milestone | Status |
|---|---|---|
| 0 | Container round-trip: parse + rebuild byte-identical (TOC opaque) | DONE 1222/1222 |
| 1 | TOC node-graph parser: account for every byte in small archives | in progress |
| 2 | Extractor (`bounty catch`): textures, scripts, geometry out as files | partial (DXT1 textures) |
| 3 | Repacker (`bounty cashin`): byte-identical rebuild, then modified rebuild | DONE - modified rebuild loads |
| 4 | **First mod loads in game** - modded loading screen confirmed in-game | **DONE 2026-07-23** |
| 5 | `.smh` blockmap format (0xBEEF2B16): level scripts extract/inject | |
| 6 | Mod system: MODS/ folder, manifest, merge, backup/restore | |
| 7 | Mod Loader GUI (NPCs, weapons, quests, textures) | |
| 8 | Community release: docs, GitHub, example mods | |

Milestone 4 was hit the same day the project started: the game accepts rebuilt
archives with no further validation (no checksums over section data). The loader
trusts the container - which means everything else on this roadmap is buildable.
