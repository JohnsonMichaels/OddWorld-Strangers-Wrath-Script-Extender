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

## Build

Requires the Visual Studio 2022+ C++ toolchain (MSVC). Run:

    build.bat

Produces `dinput8.dll` (the proxy). Install by copying it into the game's `bin\`
folder next to `stranger.exe`. Remove it to uninstall - the game reverts to
stock instantly.
