# SWSE - Feature List

The first modding framework for **Oddworld: Stranger's Wrath HD**. Everything
below is built, working, and verified in-game unless marked otherwise. The
deep technical reference for modders is [SWSE_FEATURES.md](SWSE_FEATURES.md).

## 1. Archive format library (`oddforge/`) - WORKING

The engine's `.smb` archive format, fully reverse-engineered and validated
against all 1,222 archives in the game.

- **Read + write any `.smb` archive** byte-identically (`container.py`).
  Verified: parse + rebuild reproduces every one of 1,222 archives exactly.
- **Texture enumeration** (`toc.py`): every texture in an archive with its
  name, dimensions, format, and mip count. 4,492 textures across 847 archives.
- **DXT1 decode + encode** (`dxt.py`): full texture codec with mip-chain
  support; DXT5 decode too.
- **Stat-block finder** (`stats.py`): locates editable numeric values (weapon
  configs, bounty records, melee configs) anchored to engine class names.

## 2. Mod Loader (`studio.py` / `ModLoader.exe`) - WORKING

The desktop authoring app. Standalone Windows executable, no install needed.

- **Textures tab**: browse any archive's textures, preview them, replace DXT1
  textures with your own PNG/JPG (auto-resized, re-encoded, mips rebuilt).
- **Values tab**: edit gameplay numbers - enemy weapon damage, bounty payouts
  per named character, melee configs. Labeled fields where known.
- **Mod Loader tab**: manage installed mods (section 3).
- Every edit auto-backs-up the original; one-click revert to vanilla.

## 3. Mod system (`modloader.py`) - WORKING

Multiple mods, load order, one-click apply.

- **`SWSEMods\` folder** in the game directory; each mod is a folder with a
  `mod.json` manifest plus `textures\` (drop-in replacements by game path) and
  `values.json` (stat patches).
- **Load order** (`load_order.txt`): top loads first, later mods win conflicts.
  `!ModName` disables a mod without deleting it.
- **Apply** merges all enabled mods and rebuilds only the affected archives,
  always from a clean vanilla base. **Revert** restores vanilla instantly.

## 4. SWSE - Stranger's Wrath Script Extender (`swse/`) - WORKING

A native DLL (`dinput8.dll` proxy) that loads with the game and adds what no
data edit can. Each system below can be switched off independently in
`SWSEMods\features.txt`; a disabled system installs no hooks at all.

- **Graphics pipeline**: full-scene post-processing with depth-buffer access.
  Sharpening, bloom, filmic tonemapping, colour grading, contact-shadow AO,
  screen-space RTGI, vignette. Scene-only: HUD and menus are untouched.
  F10 toggles, F11 reloads settings live.
- **In-game console** (~ key): 100+ commands covering god/heal/ammo/moolah,
  level warp, NPC spawning and relocation, live memory inspection, plus
  `list`/`call` access to the game's own script functions (348 mapped to
  native addresses, 181 callable). Scriptable from outside the game through a
  file mailbox (`tools/swse.ps1`).
- **HD texture replacement**: swaps `.oft` files in at GPU upload. 960
  textures currently shipped at 2x through an AI-upscale pipeline with
  measured seam repair and alpha-cutout detection. Archives are never touched.
- **Additive hit reactions**: NPCs flinch from the bone that was actually
  shot, on top of the game's animation. Tunable per character.
- **Foliage wind**: grass and plants sway, and part around the player as you
  walk through them. Per-plant flags in a text file (`nopush`, `sway=`).
  Implemented by rewriting the engine's ARB vertex programs at runtime;
  measured cost: no fps change.
- **NPC AI tuning**: per-character sight distance, view cone, sixth sense,
  alert decay, fire rate, reload, accuracy - driven by profiles in
  `aiprefs.txt`, off by default. The game's own difficulty setting changes
  none of these (proven; see `swse/research/AI_SYSTEMS.md`).
- **Character tuning**: per-character health, gib-on-death and hurt reactions
  from `characters.txt`, applied as each level builds its cast.
- **Self-test** (`selftest`): verifies every system is actually operational
  after each level load, and says which one broke if any did.
- **4GB patcher** (`dist_patcher/`): LAA-flags the 32-bit exe so the HD
  texture set fits in memory.

## 5. SWSE Graphics - a loadable mod - WORKING

The graphics overhaul ships as a separate mod anyone can enable or disable.
`settings.txt` exposes every effect parameter; F11 reloads it without
restarting the game.

---

## Status summary

| Capability | State |
|---|---|
| Extract/repack archives | working, byte-identical on all 1,222 |
| Texture editing (Mod Loader) | working |
| Value/stat editing (Mod Loader) | working, labeling ongoing |
| Mod folders + load order | working |
| SWSE injection + frame hook | working |
| Graphics pipeline incl. RTGI | working, scene-only via depth buffer |
| In-game console + script bridge | working, 181 native functions callable |
| HD texture replacement | working, 960 textures live |
| Hit reactions | working, on by default |
| Foliage wind + player push | working, on by default |
| NPC AI tuning | working, off by default |
| Per-system feature switches | working (`SWSEMods\features.txt`) |
| Blood decals | attempted, removed; findings in `swse/research/BLOOD_DECALS.md` |
