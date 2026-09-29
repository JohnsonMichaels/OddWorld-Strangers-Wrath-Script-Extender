# SWSE - Stranger's Wrath Script Extender

A native DLL injected into `stranger.exe` (32-bit x86, OpenGL) that adds engine
capabilities mods can use. This is the deep, native-code half of SWSE.

> **STATUS 2026-09-28: an old copy from before SWSE 1.0.** The maintained version of this README
> is `swse/README.md`, and the source and `build.bat` are in `swse/`. The `dinput8.dll` in this
> folder is the 1.0.0 build (566,784 bytes, linked 2026-07-28), and `install.bat` still installs a
> `dinput8_real.dll`, which SWSE has not needed since 1.0.1.

## Injection method: OpenGL proxy DLL

The game loads `opengl32.dll` and `dinput8.dll` from its own `bin\` before the
system copies (DLL search order). SWSE ships as a proxy that:

1. Forwards every call the game makes to the real system DLL (game runs normally).
2. On load, installs SWSE: hooks `wglSwapBuffers` (the frame-present call) so we
   own a point in every rendered frame.
3. Loads SWSE graphics/script plugins and runs their per-frame hooks.

> **CORRECTED 2026-09-28:** step 3 is not what SWSE does. On the first frame it starts the systems
> `SWSEMods\features.txt` switches on (since 1.1 only the console by default) and runs their
> per-frame work. Native plugins are designed (swse/research/PLUGIN_SYSTEM.md) and built on branch
> `feat/plugins`; they are not in 1.1 at 329df87.

We proxy `dinput8.dll` (tiny export surface - the game only needs
`DirectInput8Create`) to *get into the process*, then hook OpenGL from inside.
This keeps the proxy simple and the frame hook robust.

## Roadmap (graphics track - SSGI + post-processing, all our own shaders)

- [ ] **M1: Injection proof** - proxy loads, forwards input, draws an on-screen
      marker every frame. Confirms we control the frame. (building now)
- [ ] **M2: Framebuffer capture** - grab the game's color (and depth) buffer into
      our own FBO/texture each frame.
- [ ] **M3: Post-processing pipeline** - full-screen shader passes over the
      captured frame: tonemap, bloom, sharpen, color grade. Toggle/config via
      an SWSE mod.
- [ ] **M4: SSAO** - reconstruct view-space normals+position from depth; darken
      creases. First "real lighting" effect.
- [ ] **M5: SSGI** - screen-space global illumination: bounce light sampled from
      the color buffer. The "Dust-style RTGI look" target.

Depth-buffer access (M2) is the make-or-break step for M4/M5: SSAO and SSGI need
scene depth. If the game exposes a usable depth buffer we can bind, the lighting
effects are on. If not, we fall back to color-only effects (bloom/grade/sharpen),
which still transform the look.

> **STATUS 2026-09-28:** M1-M5 all shipped in 1.0 (2026-07-28). The depth question was answered
> yes: the scene renders into FBOs, and its depth is a sampleable texture at 2x the window size
> (swse/research/GRAPHICS_RTGI.md). The graphics work went on to GTAO, HBIL, temporal denoising
> and ray-traced AO (swse/research/GRAPHICS_ROADMAP.md, RT_1_3_PLAN.md).

## Build

Requires the Visual Studio 2022+ C++ toolchain (MSVC). Run:

    build.bat

Produces `dinput8.dll` (the proxy). Install by copying it into the game's `bin\`
folder next to `stranger.exe`. Remove it to uninstall - the game reverts to
stock instantly.

> **NOTE 2026-09-28:** `build.bat` is in `swse/`, not in this folder; it compiles every source
> file there and the version resource `swse.rc` (see swse/README.md).
