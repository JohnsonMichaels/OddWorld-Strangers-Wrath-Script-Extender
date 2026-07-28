# SWSE

**The first modding framework for Oddworld: Stranger's Wrath HD.**

by Johnson Michaels

The game never had mods because its `.smb` archive format could be unpacked but
never repacked, and it had no script hooks. SWSE changes that - it can rewrite
the game's files *and* inject code at runtime.

### [Join the Discord](https://discord.gg/TWHzP924wE)

Install help, bug reports, and a place to show off what you have built.
If you release a mod, post it in `#your-mods`.

---

## What's in the box

| Piece | What it does |
|---|---|
| **Mod Loader** | Desktop app (GUI) to browse/edit the game: import & export textures, edit stat values (bounties, damage, ...), and manage installed mods. |
| **SWSE** - Stranger's Wrath Script Extender | A DLL that loads with the game (`dinput8` proxy) and powers everything below. Each system can be switched off individually in `SWSEMods\features.txt`. |
| **SWSE Graphics** | Post-process overhaul: sharpening, bloom, filmic tonemapping, colour grading, ambient occlusion, RTGI, vignette. Press **F10** in-game, **F11** to reload settings live. Only the 3D scene is touched - HUD/menus stay clean. |
| **SWSE Console** | In-game dev console. Press **` / ~**: god, ammo, heal, transform into Steef, money, level warp, NPC control, plus `list`/`call` access to **181 of the game's own native script functions**. Scriptable from outside the game via `tools/swse.ps1`. |
| **SWSE HD** | HD texture pack: 960 textures upscaled to 2x, swapped in at GPU upload. The game's archives are never modified. |
| **SWSE Wind** | Foliage wind: grass and plants sway, and part around you as you walk through them. Per-plant tuning in `foliage.txt`. |
| **SWSE Combat** | Additive hit reactions: NPCs flinch from the bone that was actually shot. Per-character tuning in `hitreact.txt` and `characters.txt`. |
| **AI tuning** | Build your own difficulty: per-character sight, fire rate, accuracy and more via `aiprefs.txt` profiles. Off by default. |

The full command and file reference for modders is
[SWSE_FEATURES.md](SWSE_FEATURES.md).

---

## Install the mods (players)

Grab **`release/`** (or the packaged zip) and follow
**`release/INSTALL - READ ME FIRST.txt`**. Short version:

1. Copy the `bin` and `SWSEMods` folders into your Stranger's Wrath game folder.
2. Double-click `SWSE-install.bat` once.
3. Launch from Steam. **F10** = graphics, **`** = console.

Uninstall = delete `bin\dinput8.dll`, `bin\dinput8_real.dll`, and `SWSEMods`.
Saves are never touched.

---

## Mod Loader (texture / value editor)

**Run from source** (needs Python 3.10+):

```bash
pip install pillow
python studio.py
```

**Build the standalone .exe** (no Python needed to run it afterwards):

```bash
pip install pyinstaller pillow
python -m PyInstaller --onefile --windowed --name ModLoader studio.py
```

The exe appears in `dist/ModLoader.exe`. A prebuilt copy is in
[`release/ModLoader.exe`](release/ModLoader.exe) for convenience.

In Studio: **Open Archive** (or Quick Open) → browse the **Textures** tab to
export/replace textures, the **Values** tab to edit stats, or the **Mod Loader**
tab to enable/disable mods. Changes are saved back into the game with automatic
backups.

---

## Build SWSE (the script extender DLL)

Needs the Visual Studio C++ toolchain (x86). From `swse/`:

```bat
build.bat
install.bat
```

`build.bat` compiles `dinput8.dll`; `install.bat` copies it into the game's `bin`
and creates the `dinput8_real.dll` forward target. Edit the game path at the top of
`install.bat` if your install isn't the default Steam location.

---

## For modders / contributors

- **`oddforge/`** - the Python framework: `container.py` (byte-identical SMB
  repack, validated on all 1,222 archives), `dxt.py` (DXT codecs), `toc.py`,
  `textures.py`, `resize.py`, `stats.py`, `modloader.py`, `dump.py`.
- **`swse/`** - the script extender (C++): injection, the OpenGL graphics
  pipeline, the console, and `scriptvm.cpp` (the bridge that calls the game's
  native script functions).
- **`swse/research/`** - the reverse-engineering notes that make it all possible:
  the game's script VM (**348 script functions mapped to native addresses**),
  the full **reflection schema (2,014 fields)**, and which functions are
  console-callable. This is the map for building new features.
- **`FORMAT.md`** - the `.smb` container spec.
- **`tools/`** - exploration/validation scripts.

---

## Community and support

**Discord: https://discord.gg/TWHzP924wE**

| Channel | For |
|---|---|
| `#help-and-install` | Setup problems. Run `selftest` in the in-game console first and post the output - it names the system that broke. |
| `#bug-reports` | Reproducible bugs. Confirmed ones get logged as GitHub Issues, which is the permanent record. |
| `#your-mods` | Release and promote your mods. One post per mod. |
| `#mod-dev` | Building things: engine questions, reverse-engineering, tooling. |

Bugs can also go straight to
[GitHub Issues](https://github.com/JohnsonMichaels/OddWorld-Strangers-Wrath-Script-Extender/issues)
if you would rather not use Discord.

---

## Honest status

Every shipped system is verified by an in-game self-test (`selftest` in the
console) that checks each one is actually doing work, not just switched on.
One feature was attempted and removed: blood decals. The engine has no decal
system, and the findings are preserved in `swse/research/BLOOD_DECALS.md` so
nobody has to rediscover why.

Requires a legal copy of Oddworld: Stranger's Wrath HD (Steam). **No game assets
are distributed with this project.**
