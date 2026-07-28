========================================
  SWSE - Oddworld: Stranger's Wrath HD modding toolkit
========================================

The first modding framework for Stranger's Wrath. Edit textures and gameplay
values, package them as mods, and load multiple mods with a load order.

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


SAFETY
------
- SWSE always backs up original game files before changing them; use
  "Revert to Vanilla" (or delete SWSE's DLLs) to fully restore.
- Requires a legal copy of Oddworld: Stranger's Wrath HD (Steam). No game
  assets are distributed with SWSE.
