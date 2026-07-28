# SWSE - Stranger's Wrath Script Extender

A native DLL injected into `stranger.exe` (32-bit x86, OpenGL) that adds engine
capabilities mods can use. This is the deep, native-code half of SWSE.

## Injection method: OpenGL proxy DLL

The game loads `opengl32.dll` and `dinput8.dll` from its own `bin\` before the
system copies (DLL search order). SWSE ships as a proxy that:

1. Forwards every call the game makes to the real system DLL (game runs normally).
2. On load, installs SWSE: hooks `wglSwapBuffers` (the frame-present call) so we
   own a point in every rendered frame.
3. Loads SWSE graphics/script plugins and runs their per-frame hooks.

We proxy `dinput8.dll` (tiny export surface - the game only needs
`DirectInput8Create`) to *get into the process*, then hook OpenGL from inside.
This keeps the proxy simple and the frame hook robust.

## What's in it

All of the original graphics roadmap (injection, framebuffer capture,
post-processing, SSAO, SSGI/RTGI) shipped, along with systems that grew out of
it. Each one can be switched off in `SWSEMods\features.txt`:

| System | Source | Notes |
|---|---|---|
| Frame hook + graphics pipeline | `framehook.cpp`, `gfx.cpp`, `glspy.cpp` | scene-only via the depth buffer; F10 toggle, F11 live reload |
| In-game console | `console.cpp` | 100+ commands; remote mailbox for scripting from outside |
| Script-VM bridge | `scriptvm.cpp` | calls the game's own script functions; memory tooling |
| HD texture replacement | `glspy.cpp` | swaps `.oft` files at GPU upload; archives untouched |
| Additive hit reactions | `granny.cpp` | per-bone flinches from real impact data |
| Foliage wind | `foliage.cpp`, `wind.cpp` | runtime ARB vertex-program rewriting |
| NPC AI tuning | `aitune.cpp` | sight/fire-rate/accuracy profiles from `aiprefs.txt` |
| Menu callback spy | `uispy.cpp` | how the difficulty menu was decoded |
| Feature switches | `features.cpp` | per-system on/off from `features.txt` |
| Self-test | `selftest.cpp` | proves each system is doing work after level load |

The reverse-engineering notes behind all of this are in `research/` - start
with `AI_SYSTEMS.md`, `FOLIAGE_WIND.md` and `GRAPHICS_RTGI.md`. The full
command and file reference is the top-level `SWSE_FEATURES.md`.

## Build

Requires the Visual Studio 2022+ C++ toolchain (MSVC). Run:

    build.bat

Produces `dinput8.dll` (the proxy). Install by copying it into the game's `bin\`
folder next to `stranger.exe`. Remove it to uninstall - the game reverts to
stock instantly.
