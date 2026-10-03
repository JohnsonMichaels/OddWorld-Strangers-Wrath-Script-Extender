# SWSE - Stranger's Wrath Script Extender

A native DLL injected into `stranger.exe` (32-bit x86, OpenGL) that adds engine
capabilities mods can use. This is the deep, native-code half of SWSE.

## Injection method: dinput8 proxy, OpenGL hook

The game loads `opengl32.dll` and `dinput8.dll` from its own `bin\` before the
system copies (DLL search order). SWSE ships as a proxy that:

1. Forwards every call the game makes to the real system DLL (game runs normally).
2. On load, installs SWSE: hooks `wglSwapBuffers` (the frame-present call) so we
   own a point in every rendered frame.
3. On the first frame, starts whichever systems `SWSEMods\features.txt`
   switches on, and runs their per-frame work from then on.

We proxy `dinput8.dll` (tiny export surface - the game only needs
`DirectInput8Create`) to *get into the process*, then hook OpenGL from inside.
This keeps the proxy simple and the frame hook robust. The real `dinput8.dll`
is resolved at runtime: a `dinput8_real.dll` beside the proxy if there is one,
otherwise the system copy by absolute path.

## What's new in 1.1.1

- **The console's new look** (`console.cpp`, `glspy.cpp` for the font atlas upload): the
  suggestion card while a command name is typed (Tab takes one, Up/Down choose), history,
  copy and paste, colour-coded lines and the frame-rate readout. No command was added or
  changed: still 220.
- The version is 1.1.1 (`swse_version.h`); `build.bat` reads it from there.

## What's new in 1.1

- **Console-only by default.** `features.cpp` defaults every system but the
  console to off - a missing file or line means off - and nothing is removed.
  A system that is off at launch never installs its hooks.
- **Live switching.** `SWSE_FeatureStart` / `SWSE_FeatureStop` in
  `framehook.cpp` are the one place that knows how to bring each system up and
  down; launch uses them for whatever `features.txt` switches on, and the
  `features <name> on|off [temp]` console command uses them live.
- **Level watcher** (`levelwatch.cpp`): "a level is up" as its own signal - the
  player object plus a motion object it owns - instead of the hit-reaction actor
  list, which only fills with hit reactions on. AI tuning, player tuning, prefs
  edits, triggers and the self-test all key off it.
- **Player tuning** (`playertune.cpp`) and the **live prefs editor**
  (`prefsedit.cpp`): records found through the game's own resource registry
  (a sorted list of every loaded resource, binary-searched read-only), field
  names from the generated `reflect_gen.h` - 5,697 fields in 191 classes with
  exact inheritance, from `research/REFLECT_FIELDS.tsv`.
- **Facing** read and set through the engine's own `Actor::SetFacing`
  (`scriptvm.cpp`), as the matrix angle level records use, and saved with every
  position (`positions.cpp`).
- **Corrected NPC offsets** (`scriptvm.cpp`): the AIPrefs object embedded at
  `NPCPrefs+0x118`, `m_health` without `m_stamina`, and type lookups through the
  registry instead of a game call that created records on a miss.
- **The graphics line joins the main line** (the planned 1.2, folded into 1.1):
  the ray tracer (`materials.cpp`, `gpucompute.cpp`, `geocapture.cpp`,
  `raytrace.cpp`) behind its own `raytrace` switch, which needs `graphics`.
- **playnpc** (`playnpc.cpp`): play as any character in the level.
- **Menus without keys** (`menu.cpp`), **a game-build check** (`gamebuild.cpp`:
  every hook into the game's code refuses on an exe SWSE's addresses were not
  measured on),
  **background mode** (`SWSE_AGENTDEBUG=1`, `mute.cpp`), the tool contract
  (`query contract`), and a log that rotates at 4 MB and names each session.
- One version number in `swse_version.h`, used by the console, `query version`,
  the log banner and `swse.rc`.
- 41 new console commands: 220 in all (the count `help` prints; 1.0.2 had 179), not counting plugins' own.
  Among them `yaw`, `tpxyz`, `playertune`, `prefs`, `knockback`, `npc`, `wpn`,
  `bind`/`unbind`/`binds`, `alias`/`unalias`/`aliases`, `after`, `wait`,
  `repeat`, `exec`, `log`, `status`, `query`, `mods`, `hide`, `show`,
  `npccache`, `mute`, `playnpc`, and the graphics line's `rt`, `harvest`, `geo`,
  `gpu`, `progsrc`, `ssrmask`, `skin`, `meshdump`, `hidepart`, `noponcho`,
  `nohat` and `nodreads`.

## What's in it

All of the original graphics roadmap (injection, framebuffer capture,
post-processing, SSAO, SSGI/RTGI) shipped, along with systems that grew out of
it. Each one has a switch in `SWSEMods\features.txt`; only the console is on by
default:

| System | Source | Switch | Notes |
|---|---|---|---|
| Proxy entry | `dllmain.cpp` | - | finds the real `dinput8.dll`, forwards its other exports, and starts SWSE (`DirectInput8Create` itself is in `input.cpp`) |
| Frame hook | `framehook.cpp` | - | `wglSwapBuffers` hook; starts and stops every system (`SWSE_FeatureStart/Stop`) |
| Graphics pipeline | `gfx.cpp`, `glspy.cpp` | `graphics` | scene-only via the depth buffer; F10 toggle, F11 live reload |
| Ray tracing (experimental) | `materials.cpp`, `gpucompute.cpp`, `geocapture.cpp`, `raytrace.cpp` | `raytrace` (needs `graphics`) | draw hook and material mask (installed with `graphics`), geometry harvest, BVH, GPU tracer, RTAO |
| In-game console | `console.cpp` | `console` | 220 commands; remote mailbox, binds, aliases, sequences |
| Script-VM bridge | `scriptvm.cpp` | - | calls the game's own script functions; player, motion, facing, memory tooling |
| HD texture replacement | `glspy.cpp` | `hdtextures` | swaps `.oft` files at GPU upload; archives untouched |
| Additive hit reactions | `granny.cpp` | `hitreact` | per-bone flinches from real impact data |
| Foliage wind | `foliage.cpp`, `wind.cpp` | `foliage` | runtime ARB vertex-program rewriting |
| NPC AI tuning | `aitune.cpp` | `aituning` | sight/fire-rate/accuracy/miss-time profiles from `aiprefs.txt` |
| Triggers | `triggers.cpp` | `triggers` | mod-defined events from `triggers.txt`; level and NPC counts no longer need hit reactions (1.1) |
| Character tuning | `scriptvm.cpp` | `npctuning` | `characters.txt` + `console.txt` rules as characters spawn |
| Player tuning | `playertune.cpp` | `playertune` | `playerprefs.txt`: health, stamina, speed, jump, gravity, air control (1.1) |
| Live prefs editor | `prefsedit.cpp`, `reflect_gen.h` | `prefsedit` | read/write any loaded prefs record; `prefs.txt` per level (1.1) |
| Level watcher | `levelwatch.cpp` | - | the "a level is up" epoch the per-level appliers and the self-test use (1.1) |
| Named positions | `positions.cpp` | - | `positions.txt` / `sites.txt`, with facing (1.1) |
| Play as a character | `playnpc.cpp` | - | `playnpc`; off until used (1.1) |
| Menus without keys | `menu.cpp` | - | `menu list/continue/skip/resume/fs`: sends a Flash screen its own command (1.1) |
| Native plugins | `plugins.cpp` (host side of `..\sdk\swse_plugin_api.h`) | one switch per plugin, off until named | loads `SWSEMods\<Mod>\plugins\<name>.dll`, fault guard, `plugins`, `query plugins` (1.1) |
| Hook registry | `hookreg.cpp` | - | every code/table patch SWSE has live, by owner; `hooks`; a plugin's `Memory.Write` refuses those ranges (1.1) |
| Free camera | `freecam.cpp` | - | `freecam`: flies the game's own developer FlyCamera (1.1) |
| Menu callback spy | `uispy.cpp` | - | `uispy on` logs each command a menu screen receives (the real handlers, 19 screens) |
| Game-build check | `gamebuild.cpp` | - | identifies the exe from its PE header; an unknown build runs in safe mode: no game patch, call or read (the gates: `SWSE_GameBuildSafeMode`, console.cpp's `SafeModeBlocks`) (1.1) |
| Input | `input.cpp` | - | `DirectInput8Create` and the device hooks, key injection, and the user32 imports background mode answers |
| Audio mute | `mute.cpp` | - | `mute`: the game's own per-app mute; background mode uses it (1.1) |
| Shader reconnaissance | `shaderspy.cpp` | - | `shaderdump`: every shader program the game built, to a file |
| Mod registry | `modregistry.cpp` | - | any folder under `SWSEMods` provides files; `load_order.txt` |
| Feature switches | `features.cpp` | - | per-system on/off from `features.txt`, saved by the `features` command |
| Self-test | `selftest.cpp` | - | proves each system is doing work, once per level |

`reflect_gen.h` is generated - its generator (`tools/gen_reflect_header.py`, from
`research/REFLECT_FIELDS.tsv`, which `tools/reflect/` extracts from `stranger.exe`) is in the
development repository, not this one - so do not edit it by hand.
`scriptvm_gen.h`, the 181 script verbs `list` and `call` offer, is generated too,
from the decoded signatures (`research/script_signatures.tsv`,
`CONSOLE_CALLABLE.md`); its generator (`gen_cmd_table.py`, named in the file's
header) is not in the repository.

The reverse-engineering notes behind all of this (the research index, the
engine map, PLAYER_FACING.md, PREFS_EDITOR.md, REFLECT_FIELDS.tsv, PLAYNPC.md,
AT3_DISCOVERIES.md and the rest) are kept in the development repository; only
`research/REFLECTION_SCHEMA.md` ships here, because the Mod Loader reads it.
The full command and file reference is the top-level `SWSE_FEATURES.md`,
release history is `CHANGELOG.md`, `TOOL_CONTRACT.md` is what tools can rely
on, and `AT3_INTEGRATION.md` is the guide for tools built on SWSE.

> **Not in this repository.** The same goes for every research note named in
> these docs and in code comments (`swse/research/*.md`, `*.tsv`) and for the
> `tools\` scripts: they are kept in the development repository. This public
> snapshot ships the source, the plugin SDK, the Mod Loader and the release
> files.

## Build

Requires the Visual Studio 2022+ C++ toolchain (MSVC). Run:

    build.bat

Run it from PowerShell or a Command Prompt; `cmd /c build.bat` from a Bash
shell can hang. It compiles `swse.rc`, then every `.cpp` in this folder (32
files, x86, `/O2 /EHsc`) into one DLL; a new source file must be added to its
`cl` line.

Produces `dinput8.dll` (the proxy), with the version resource from `swse.rc`
(numbers from `swse_version.h`: file version 1.1.1.0 for SWSE 1.1.1) so tools
can read the installed version without running the game. Install by copying it into the game's `bin\` folder next to
`stranger.exe`. Remove it to uninstall - the game reverts to stock instantly.
No `dinput8_real.dll` is needed: the proxy loads the system `dinput8.dll` by
absolute path. Its exports are declared in `dllmain.cpp` and `input.cpp`, not
in a `.def` file. `install.bat` copies the DLL into a default Steam install
and still stashes a `dinput8_real.dll`, which is harmless. The release zip is built by
`tools/package_swse.ps1` in the development repository.
