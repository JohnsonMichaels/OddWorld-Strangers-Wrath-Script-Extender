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

## What's new in 1.1.1

* **The console's new look**: sharp text at any resolution, the frame rate
  (fps and ms) at the top right, and colour-coded commands, results and
  errors.
* **Typing help**: suggestions as you type a command name (Tab takes one,
  Up/Down choose), and Up/Down history of what you typed.
* **Copy and paste**: Ctrl+V or Shift+Insert pastes (each pasted line runs),
  Ctrl+C copies the line you are typing - or, with nothing typed, the last
  command and its output - and Ctrl+Shift+C copies the lines on screen.
* Nothing else changed: the same 220 commands, switches and defaults as 1.1.
  The keys in full:
  [The console's look and keys](SWSE_FEATURES.md#the-consoles-look-and-keys-111).

---

## What's new in 1.1

* **Only the console is switched on after installing.** Every other system is
  still there - nothing was removed - but it stays off until you turn it on,
  so a fresh install leaves the game exactly as shipped apart from the
  console. 1.0.x switched everything on.
* **Switch systems on and off while you play**: `features foliage on` in the
  console switches wind on now and remembers it; add `temp` to try something
  for one session.
* **Player tuning** - your health, stamina, run speed, jump, gravity and air
  control from `playerprefs.txt`, every level.
* **A live prefs editor** - change any of the game's own values (ammo
  knockback, weapon timing, motion...) while it runs, from the console or a
  `prefs.txt`, without touching a game file. `knockback boombat 40 25` makes
  boombats throw what they hit harder and sets how hard they throw you.
* **Your facing is saved** with every position (`savepos`, `writepos`), and
  restored by `tp` / `goto` - as the same angle the game's level files use.
* **Console automation**: key binds, aliases, `;` to run several commands in
  one line, `wait`, `after`, `repeat`, `exec`.
* **Play as any character in the level** (`playnpc`), with its own body and
  animations.
* **Ray-traced ambient occlusion, as an experiment** - the `raytrace` switch,
  off by default and only with graphics on. Its occlusion is known to be
  wrong: it only sees what the camera already drew.
* **A game-build check, and a safe mode**: on a build SWSE's addresses were
  not measured on (GOG, a future patch), nothing that patches, calls or reads
  the game runs, so it cannot crash it; the console, graphics, HD textures,
  foliage and ray tracing still work.
* **For tool authors**: `query` answers in key=value, `status`, `mods`, menus
  driven without key presses, a background mode (`SWSE_AGENTDEBUG=1`), a
  written [tool contract](TOOL_CONTRACT.md), and support for what
  [Stranger: Armed to the Teeth](AT3_INTEGRATION.md) expects.
* Several long-standing bugs fixed along the way - see
  [CHANGELOG.md](CHANGELOG.md).

---

## What's in the box

| Piece | What it does | Switch (`features.txt`) |
|---|---|---|
| **SWSE Setup** | The players' installer (`SWSE Setup.exe` in the zip): installs, updates or removes SWSE and switches its systems with tick boxes. | - |
| **Mod Loader** | The modders' desktop studio (`studio.py`, built from this repository, not in the player zip): import & export textures, edit stat values (bounties, damage, ...), manage installed mods - and the same SWSE Setup screen as its first tab. | - |
| **SWSE** - Stranger's Wrath Script Extender | A DLL that loads with the game (`dinput8` proxy) and powers everything below. Each system is switched on or off individually in `SWSEMods\features.txt`, or live with `features <name> on\|off`. | - |
| **SWSE Console** | In-game dev console. Press **` / ~**: god, ammo, heal, transform into Steef, money, level warp, NPC control, playing as any character in the level (`playnpc`), plus `list`/`call` access to **181 of the game's own native script functions**. Scriptable from outside the game through the remote mailbox (`SWSE Console\remote_in.txt` - see [AT3_INTEGRATION.md](AT3_INTEGRATION.md#the-remote-mailbox)). | `console` - **on** |
| **SWSE Graphics** | Post-process overhaul: sharpening, bloom, filmic tonemapping, colour grading, ambient occlusion, RTGI, vignette. Once switched on, press **F10** in-game for the look, **F11** to reload settings live. Only the 3D scene is touched - HUD/menus stay clean. | `graphics` - off |
| **SWSE HD** | HD texture pack: 960 textures upscaled to 2x, swapped in at GPU upload. The game's archives are never modified. The pack itself is a separate download (about 200 MB, `SWSE-HD-Textures-2x.zip` on the [1.0 release](https://github.com/JohnsonMichaels/OddWorld-Strangers-Wrath-Script-Extender/releases/tag/v1.0), unchanged since); it needs the included 4GB patcher, and the INSTALL file walks through both. | `hdtextures` - off |
| **SWSE Wind** | Foliage wind: grass and plants sway, and part around you as you walk through them. Per-plant tuning in `foliage.txt`. | `foliage` - off |
| **SWSE Combat** | Additive hit reactions: NPCs flinch from the bone that was actually shot. Tuning in `hitreact.txt`. | `hitreact` - off |
| **AI tuning** | Build your own difficulty: per-character sight, fire rate, accuracy, miss time and more via `aiprefs.txt` profiles. | `aituning` - auto |
| **Triggers** | Mod-defined game events from a mod's own `triggers.txt`. | `triggers` - off |
| **Character tuning** | Per-character health and gib rules from `characters.txt` and `console.txt`, every level. | `npctuning` - off |
| **Player tuning** (1.1) | Your own health, stamina, speed, jump, gravity and air control from `playerprefs.txt`, every level. | `playertune` - auto |
| **Prefs editor** (1.1) | Live edits to any of the game's loaded prefs records from `prefs.txt` or the console (`prefs`, `knockback`). | `prefsedit` - off |
| **Ray tracing** (1.1, experimental) | Ray-traced ambient occlusion inside the graphics pipeline. Not finished: it only sees what the camera already drew. | `raytrace` - off (needs `graphics`) |

`auto` means the system runs only while its own file asks for something - an
`active =` profile in `aiprefs.txt`, or a value in `playerprefs.txt`. A fresh
install asks for nothing, so it stays console-only.

The full command and file reference for modders is
[SWSE_FEATURES.md](SWSE_FEATURES.md); release history is
[CHANGELOG.md](CHANGELOG.md).

---

## Install the mods (players)

Download **`SWSE-1.1.1.zip`** from the
[Releases page](https://github.com/JohnsonMichaels/OddWorld-Strangers-Wrath-Script-Extender/releases)
and follow **`INSTALL - READ ME FIRST.txt`** inside it. (The repository's
`release/` folder holds the same files except `SWSE Setup.exe`, which is
built from `swse_setup.py` - see below.)

**The easy way:** unzip, run **`SWSE Setup.exe`**, tick what you want on (or
press **Recommended**, **Classic SWSE** or **Everything**) and press
**Install / Update**. It finds the game (Steam or GOG), copies SWSE in without
touching your own files, and writes `SWSEMods\features.txt` from the ticks.
**Apply switches** changes them later - live, through the console mailbox,
when the game is running.

The manual way:

1. Copy the `bin` and `SWSEMods` folders into your Stranger's Wrath game folder.
2. Launch from Steam. **`** = console. (`SWSE-install.bat` is optional since
   1.0.1: SWSE finds the system `dinput8.dll` by itself.)
3. Switch on what you want: `features preset full` in the console gives the
   classic SWSE (graphics, HD textures, hit reactions, foliage and AI tuning on)
   in one word. Or change `off` to `on` at the top of `SWSEMods\features.txt`,
   or type `features <name> on` - for example `features graphics on`, then
   **F10** for the look. `features` on its own lists everything.
4. Optional: for the HD texture pack, download `SWSE-HD-Textures-2x.zip` from the 1.0 release on the Releases page,
   unzip into `SWSEMods\SWSE HD\textures\`, run `SWSE_4GB_Patcher.exe` once,
   and switch on `hdtextures`. The INSTALL file explains why the patch is
   needed and how to undo it.

Uninstall = SWSE Setup's **Uninstall** (it moves `bin\dinput8.dll` and
`SWSEMods` into a dated folder in the game folder, deleting nothing), or by
hand: delete `bin\dinput8.dll`, `bin\dinput8_real.dll` (if you created it),
and `SWSEMods`. Saves are never touched.

---

## SWSE Setup (the players' installer)

`SWSE Setup.exe` in the release zip, from `swse_setup.py`: one window, the
Setup screen of `oddforge/setupui.py` - the same screen the Mod Loader shows as
its first tab. It sits beside `bin\` and `SWSEMods\` in the unzipped
download and installs from there.

```bash
python swse_setup.py                                        # from source
pip install pyinstaller
python -m PyInstaller SWSESetup.spec --distpath release     # -> release\SWSE Setup.exe
```

The release zip ships `SWSE Setup.exe` (git-ignored here; about 12 MB, as it
needs no Pillow). The packaging script refuses an exe older than the Python
it is built from, and runs its `--selftest` before zipping. What SWSE Setup
does:
- It finds the game from `SWSE_GAME_DIR`, every Steam library, and the GOG
  registry and folders - with **Browse** to pick another - and shows which
  SWSE is installed there (`bin\dinput8.dll`'s version).
- A tick box per switch with its one-line description (aituning and playertune
  get auto/on/off), plugins found in the enabled mods listed after them, and
  the presets **Recommended** (the 1.1 default), **Classic SWSE**
  (`features preset full`) and **Everything**.
- **Install / Update** copies `bin\dinput8.dll` and `SWSEMods` from the folder
  it runs in (the unzipped release), then writes `features.txt`
  from the ticks. It never overwrites the user's files (binds, aliases, sites,
  prefs, playerprefs and positions in SWSE Console, `features.txt`,
  `load_order.txt`, a mod folder they added) or an `aiprefs.txt` that differs
  from ours; a shipped file they edited goes to `SWSE-backup-<date>` in the
  game folder before the new one replaces it. It refuses while the game runs.
- **Apply switches** rewrites only `features.txt` (values only, comments
  kept), and with the game running also sends `features <name> on|off` through
  the console mailbox so the change applies at once.
- **Uninstall** moves `bin\dinput8.dll` and `SWSEMods` into
  `SWSE-uninstalled-<date>` in the game folder. Nothing is deleted.
- Ticking HD textures shows whether the pack is in `SWSE HD\textures` and
  offers `SWSE_4GB_Patcher.exe` when the exe still needs the patch.

The logic is in `oddforge/swsefeatures.py` (features.txt, read and written by
`swse/features.cpp`'s rules), `oddforge/swseinstall.py` and
`oddforge/gamepaths.py`. `"SWSE Setup.exe" --selftest <file>` (and
`ModLoader.exe --selftest <file>`) builds the window without showing it and
reports whether the exe finds its modules and the release beside it.

---

## Mod Loader (the modders' studio: texture / value editor)

Not in the player zip: build it from this repository.

**Run from source** (needs Python 3.10+):

```bash
pip install pillow
python studio.py
```

**Build the standalone .exe** (no Python needed to run it afterwards):

```bash
pip install pyinstaller pillow
python -m PyInstaller ModLoader.spec
```

The spec builds one windowed exe and bundles
`swse/research/REFLECTION_SCHEMA.md`, which the **Game Data** tab uses to name
fields. The exe appears in `dist/ModLoader.exe`.

Its first tab is the SWSE Setup screen above. In Studio: **Open Archive** (or
Quick Open) → browse the **Textures** tab to export/replace textures, the
**Game Data** tab to edit stats, or the **Mod Loader** tab to enable/disable
mods and set their load order. Changes are saved back into the game with
automatic backups.

A Mod Loader mod is a folder under `SWSEMods\` with a `mod.json`, plus
`textures\<in-game path>.png` replacements and/or a `values.json` of stat
patches; **APPLY ALL MODS** rebuilds the affected archives and **Revert to
Vanilla** undoes it. (A `textures\*.oft` file is different: the SWSE DLL swaps
those in at runtime, by fingerprint, and never touches an archive.)

---

## Build SWSE (the script extender DLL)

Needs the Visual Studio C++ toolchain (x86). From `swse/`:

```bat
build.bat
install.bat
```

`build.bat` compiles `dinput8.dll`; `install.bat` copies it into the game's `bin`
and creates the optional `dinput8_real.dll` beside it. Edit the game path at the
top of `install.bat` if your install isn't the default Steam location.

---

## For modders / contributors

- **`oddforge/`** - the Python framework: `container.py` (byte-identical SMB
  repack, validated on all 1,222 archives), `dxt.py` (DXT codecs), `toc.py`,
  `textures.py`, `resize.py`, `stats.py`, `modloader.py`, `dump.py`.
- **`swse/`** - the script extender (C++): injection, the OpenGL graphics
  pipeline, the console, `scriptvm.cpp` (the bridge that calls the game's
  native script functions), and in 1.1 the level watcher, player tuning, the
  live prefs editor, `playnpc`, the game-build check and the experimental ray
  tracer. See [swse/README.md](swse/README.md).
- **`swse/research/`** - in this repository, `REFLECTION_SCHEMA.md`, which the
  Mod Loader's Game Data tab uses to name fields. The full reverse-engineering
  notes (the game's script VM with 348 script functions mapped, the exact
  reflection table of 5,697 fields in 191 classes, which functions are
  console-callable) are kept in the development repository, not published
  here.
- **[AT3_INTEGRATION.md](AT3_INTEGRATION.md)** - building a tool on SWSE: the
  files, the remote mailbox and `query`, as used by Stranger: Armed to the
  Teeth. **[TOOL_CONTRACT.md](TOOL_CONTRACT.md)** is what a tool can rely on
  in every 1.x release.
- **`FORMAT.md`** - the `.smb` container spec.
- **`sdk/`** - the native plugin SDK: the C header, a complete example and a
  stand-in host. See [sdk/README.md](sdk/README.md).

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

The core systems - frame hook, graphics, HD textures, foliage wind and hit
reactions - are verified by an in-game self-test (`selftest` in the console)
that checks each one is actually doing work, not just switched on. It also
checks the game build, the level watcher, and each tuning system and the ray
tracer that is switched on; `difficulty`, `playertune`, `prefs status` and
`triggers` give the details. Systems you have not switched on are listed on
one `[OFF ]` line, never as a failure. One feature was
attempted and removed: blood decals. The engine has no decal system; the
findings are kept in the project's research notes so nobody has to
rediscover why.

Requires a legal copy of Oddworld: Stranger's Wrath HD (Steam). The GOG
release is not supported yet: SWSE recognises it as a different build and
runs in safe mode there, keeping out of the game rather than crashing it. **No original
game files are distributed with this project.** The optional HD texture pack
on the Releases page contains AI-upscaled derivatives of the game's own
textures; it is useless without the game and exists for people who own it.
