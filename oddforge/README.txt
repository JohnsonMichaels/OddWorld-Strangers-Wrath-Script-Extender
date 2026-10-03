========================================
  SWSE - Oddworld: Stranger's Wrath HD modding toolkit
========================================

The first modding framework for Stranger's Wrath. Edit textures and gameplay
values, package them as mods, and load multiple mods with a load order.

NOTE 2026-09-28: this README dates from before SWSE 1.0 (July 2026). It
describes a distribution folder: ModLoader.exe is built from studio.py with
ModLoader.spec and is not in git. The SWSE section below is out of date; see
the notes in it. Current references: README.md, SWSE_FEATURES.md and
CHANGELOG.md at the repository root.

WHAT'S IN THIS FOLDER
---------------------
ModLoader.exe   The app. Double-click to run. No install needed.
SWSE/                Stranger's Wrath Script Extender (native graphics/effects engine).
ExampleMods/         Two ready-made mods to copy in as templates.
docs/                Design docs (format spec, roadmap, architecture).


QUICK START - EDITING
---------------------
1. Run ModLoader.exe.
2. TEXTURES tab: "Open Archive..." (or "Quick Open"), pick a texture, replace it
   with your own PNG, then "Save Modded Archive". Originals are auto-backed up.
3. VALUES tab: "Quick Open" -> a weapon-stats archive or global_prefs.smb.
   Edit numbers (bounty payouts, weapon/melee values), Stage, Save.


QUICK START - MODS (multiple mods + load order)
-----------------------------------------------
A mod is a folder under:
   ...\Steam\steamapps\common\Stranger's Wrath\SWSEMods\
Each mod folder has a mod.json plus:
   textures\<in-game path>.png     (texture replacements)
   values.json                     (stat patches)

1. Copy the folders in ExampleMods\ into your game's SWSEMods\ folder.
2. In ModLoader.exe -> MOD LOADER tab: reorder, enable/disable, then
   "APPLY ALL MODS". "Revert to Vanilla" undoes everything.


SWSE - GRAPHICS/EFFECTS ENGINE (advanced, in progress)
------------------------------------------------------
SWSE is a script extender that will run custom OpenGL effects (sharpening,
bloom, ambient occlusion, screen-space global illumination) with an in-game
menu (Home key). It ships effects as the "SWSE Graphics" mod.

Status: SWSE currently INJECTS and detects enabled graphics mods. The shader
rendering pipeline and in-game menu are still in development, so effects are
not visible yet. To test injection:
   1. Run SWSE\install.bat (copies dinput8.dll into the game's bin\).
   2. Launch the game once, then check bin\swse_log.txt.
   To uninstall: delete dinput8.dll and dinput8_real.dll from bin\.

CORRECTED 2026-09-28: all of this section has moved on.
- The graphics pipeline shipped in 1.0 (2026-07-28). Since 1.1 it is off by
  default: type "features graphics on" in the console, then F10 shows the
  effect and F11 reloads graphics.txt.
- There is no Home-key menu. The in-game interface is the console: the ` / ~
  key opens it (SWSE_FEATURES.md, "Driving the console").
- dinput8_real.dll is not needed since 1.0.1: SWSE finds the system
  dinput8.dll itself. Uninstall by deleting bin\dinput8.dll.
- The dinput8.dll in oddforge\SWSE\ is the SWSE 1.1.1 build, the same file as
  release\bin\dinput8.dll. To build it yourself: swse\build.bat.


SAFETY
------
- SWSE always backs up original game files before changing them; use
  "Revert to Vanilla" (or delete SWSE's DLLs) to fully restore.
- Requires a legal copy of Oddworld: Stranger's Wrath HD (Steam). No game
  assets are distributed with SWSE.

NOTE 2026-09-28: the backups are the Mod Loader's (archive edits). The SWSE
DLL changes no game file: it edits the running game in memory. SWSE's setup
and the Mod Loader can also find a GOG install (oddforge/gamepaths.py), but the
GOG build is untested, and most of the DLL's in-game code patches refuse on any
build other than Steam HD.
