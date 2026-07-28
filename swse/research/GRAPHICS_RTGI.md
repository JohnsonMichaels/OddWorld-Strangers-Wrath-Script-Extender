# Screen-space GI on Stranger's Wrath HD - what actually blocked it

Depth-based GI (SSAO / SSGI / "RTGI") produced flat, mediocre output for a long
time, and the working theory was that the engine exposed no usable depth, or no
normals. **Both were wrong.** The engine hands us everything needed. Four
separate defects downstream destroyed the signal.

This document records the measurements so nobody re-derives them.

## The rendering pipeline

The game is OpenGL (GL2 / Cg era), not D3D. SWSE hooks `wglSwapBuffers` and
runs its post-process there.

| Fact | Value | How it was measured |
|---|---|---|
| Default framebuffer depth | **`depthBits = 0`** | `DEPTHPROBE` in `framehook.cpp` |
| Scene rendering target | an **FBO** | 2536 `glBindFramebufferEXT` calls |
| Depth attachment type | **`0x1702` = `GL_TEXTURE`** | `glGetFramebufferAttachmentParameterivEXT` |
| Depth texture size | **3360x2100** for a 1680x1050 window | `glGetTexLevelParameteriv` |

The default framebuffer genuinely has no depth buffer, which is what made it
look like depth was unavailable. The game renders the scene into FBOs and blits
only colour to the window. Depth lives in a **sampleable texture** the whole
time, at **2x supersampling**.

> Note: `glspy` logs to **`swse_glspy.txt`**, not `swse_log.txt`. Reading the
> wrong file is what produced the false "the game never binds an FBO" reading.

## Defect 1 - the depth buffer was inverted (this was the fatal one)

The shader did `1.0 - texture2D(uDepth, ...).r`, i.e. it assumed **reverse-Z**.
This engine uses **standard** GL depth (near = 0, far = 1).

The visible scene occupies raw depth **0.96 .. 0.99** - about 3% of the range,
which is normal for a perspective buffer with a close near plane. Linearising
with the real near/far:

| | raw 0.9647 | raw 0.9961 |
|---|---|---|
| correct (no invert) | z = **13.97** | z = **113.6** |
| inverted (old code)  | z = **0.502** | z = **0.518** |

Inverted, the entire frame collapses into a ~3% spread of z. Every normal is
reconstructed by differencing neighbouring depths, so with a constant depth the
normals are pure noise and AO saturates over the whole image. In the `debug_gi`
view this appeared as **uniform red across the frame**.

## Defect 2, 3, 4 - all three camera constants were guessed

`settings.txt` carried hand-tuned values. None matched the real camera:

| Constant | Guessed | Real | Error |
|---|---|---|---|
| `depth_far` | 120 | **1000** | 8.3x |
| `depth_near` | 1.0 | **0.5** | 2x |
| `fov` | 65 deg | **80 deg** | 23% |

Near/far set the depth linearisation; FOV scales the view-space ray
reconstruction. Wrong values skew every reconstructed normal even once the
inversion is fixed.

## Getting the real camera

`glGetFloatv(GL_PROJECTION_MATRIX)` is **useless here** - the only matrix ever
observed is the identity, because the engine feeds matrices to its shaders
itself rather than using the fixed-function stack. (Verified with `PROJMAT`
logging.)

Instead the engine reflects a frustum struct, found by value signature:

```
+0x00  m_zNear      +0x04  m_zFar      +0x08  m_fovRad   ...   +0x6C  m_tanHalfFov
```

Two things make the scan reliable rather than a guess:

1. **The `tanHalfFov` cross-check.** It must agree with `tan(fovRad/2)`, which
   rejects essentially every coincidental float triple.
2. **Pick the mode, not the maximum.** The engine keeps many copies of the live
   frustum for its various passes, so the true camera is the value that
   *repeats*; junk matches are one-offs. Selecting "widest frustum" instead
   picked a lone outlier (`near=1.009 far=1449`) over the real camera.

Console: `proj` reports the camera, `proj scan` lists all candidates.

The frustum differs per level - `0.5 / 1000 / 80deg` in the intro area,
`0.15 / 1000 / 90.39deg` in the town save. It is re-acquired automatically
(throttled to one scan per 5s while unknown).

## The V-flip was NOT a bug

The game's depth attachment **is** mirrored vertically relative to the frame
captured with `glReadPixels`. `depth_flipv = 1` is correct. With it off,
geometry ghosts into the image upside down - clearly visible in `debug_gi`.

This one was worth A/B-ing rather than reasoning about: the reasoning said the
flip was wrong, the measurement said otherwise.

## Switches

Both depth conventions are live-tunable, so another build or driver can be
adapted without a rebuild:

```
set depth_invert 0      # standard depth, NOT reverse-Z   (verified)
set depth_flipv  1      # depth is V-flipped vs the frame (verified)
set debug_gi     1      # red = occlusion, green = bounced light
```

`debug_gi` is the fastest way to tell a real problem from a tuning problem:
if it shows scene structure, the geometry pipeline is sound and everything
left is intensity tuning.

## The "grid texture" artifact

Once the depth was fixed, a fine diagonal grid became visible across the image.
That is **not** an intensity problem - it is sampling noise.

The AO loop rotates its sample ring by a **per-pixel** random angle,
`ign(gl_FragCoord.xy)`, and SSGI seeds its ray directions the same way. `ign` is
interleaved gradient noise, whose error pattern is a regular diagonal lattice.
Real SSAO hides this with a blur pass over the AO term; this shader has **no
denoise pass at all**, so the dither is composited straight into the frame.

Two mitigations are in place:

- `ao_samples` (4..32) is now tunable - variance falls as 1/N, so raising it
  attacks the artifact directly. At 8 the grid is obvious; at 32 it is gone.
- The AO ring now uses a **golden-angle spiral** (`i * 2.39996`) instead of a
  linear sweep, so the same sample budget covers the disc far more evenly.

The proper fix is still a real denoise: render AO to its own texture and run a
depth-aware (bilateral) blur before compositing. That needs an intermediate
render target, which this one-pass design deliberately avoided.

## Silhouette halos ("crown of thorns")

Characters and the player threw a dark aura onto the ground around them, with
radial streaks off their silhouettes.

Cause: the SSGI hit test was `zs < zray - 0.05` - **any** nearer surface counted
as a blocker, with no upper bound. A depth buffer only stores the *front*
surface, so a character standing in the foreground occluded rays cast by ground
pixels far behind it, smearing its silhouette outward. (The AO loop bounded this
correctly with `diff < uAORadius*3.0`; SSGI did not.)

Fix: a **thickness test**. A hit only counts if the occluder is plausibly thick;
anything further in front is something the ray should pass behind. Both bounds
scale with distance, because a fixed world-space epsilon is far too tight at the
far end of a 0.5..1000 range:

```glsl
float dz    = zray - zs;
float bias  = max(0.05, z0*0.01);
float thick = uSSGIThickness * max(1.0, z0*0.05);
if (dz > bias && dz < thick) { /* real occluder */ }
```

Verified with an amplified difference image: only **2.5%** of pixels changed and
they sat entirely on the characters and the ground immediately around them - the
buildings, street and sky were untouched. That is what a surgical silhouette fix
should look like, and it is far more convincing than eyeballing two screenshots
of a scene where NPCs walk between captures.

`ssgi_thickness` is tunable. Setting it very high reproduces the old unbounded
behaviour exactly, which makes it a free A/B switch.

## Tuning

With the geometry correct, the intensities that had been dialled down to hide
garbage were suddenly far too strong:

| Setting | Too strong | Tuned | Symptom when too high |
|---|---|---|---|
| `ao_intensity` | 1.8 | **0.9** | reads as over-sharpened; speckle on rock faces |
| `ao_radius` | 1.5 | **1.2** | same |
| `ssgi_intensity` | 2.0 | **0.8** | white rim around characters, emissive signs blow out |

The white rim is a direct consequence of the thickness fix: rays that used to be
*blocked* by a foreground character now pass behind it and hit the bright ground
or sky, so they bounce back more light.

At the tuned values the pass still changes **81.8%** of pixels versus vanilla
(mean 9.59/255), so the effect is emphatically still there - only the excess was
removed.

## A bug in the settings writer (fixed)

`SWSE_GfxSetSetting` destroyed settings.txt over repeated use:

1. **CR accumulation.** The file is CRLF, but the splitter only looked for
   `'\n'`, so every line kept its trailing `'\r'` and was written back as
   `"%s\r\n"` → `"...\r\r\n"`. Each `set` added one more CR to every line; after
   ~25 calls the file had ~25 blank lines between each real line.
2. **Unbounded write.** `o += wsprintfA(out + o, ...)` never checked its buffer,
   so once the CRs inflated the text past 9216 bytes it smashed the stack. The
   8192-byte read cap silently truncated the tail as well.

Now CR is stripped when splitting, every append is bounds-checked, and the
buffers are 64K. Verified: 10 consecutive `set` calls leave the line count
unchanged at 75 and preserve all comments.

## UI and the first-person weapon get shaded with the world behind them

Symptom: scenery ghosts through the inventory poster (a picket fence and posts
were clearly visible painted onto it), and the crossbow looks like the ground is
bleeding through it.

Cause, confirmed by diffing the inventory screen with the effect on vs off -
**73.9% of pixels change, including all of the UI**. UI and the first-person
weapon are drawn *after* the depth pass, so at those pixels the depth buffer
still holds the world behind them. Our AO/GI then computes the *world's*
occlusion and paints it onto the UI.

Measured: the nearest surface in the scene depth texture is ~4.7 units, while a
first-person weapon sits under 2 - so the weapon is genuinely absent from the
depth we sample.

### The frame structure (measured with `fbotrace`)

Every frame is identical:

```
26 x bind fbo=1     <- all 3D scene rendering
 1 x bind fbo=0     <- switch to the backbuffer: composite + UI
     SWAP           <- the post-process currently runs HERE, too late
```

The game reuses a single FBO id and rebinds it with different attachments, which
is why several depth texture names appear under one FBO. There is exactly one
`fbo=0` transition per frame and it is unambiguous: everything after it is
composite plus UI.

### Attempted fix: run the pass at the fbo=0 transition - FAILED, do not retry as-is

The idea was to post-process the scene while its FBO was still bound, before
any UI existed. It **crashed the game**, and the reason is instructive.

Extending `fbotrace` to log the ending pass's viewport and colour attachment:

```
FBOSEQ: 0  ending-vp=1680x1050  colorType=0x1702 colorName=7  -> bind fbo=1
FBOSEQ: 1  ending-vp=1680x1050  colorType=0x1702 colorName=7  -> bind fbo=1
FBOSEQ: 2  ending-vp=6720x4200  colorType=0x0    colorName=0  -> bind fbo=0
```

The pass immediately before `fbo=0` is **depth-only**: no colour attachment at
all (`colorType=0`), at a 6720x4200 viewport. Calling `glCopyTexSubImage2D` on a
framebuffer with no colour buffer is undefined - it produced a blurry frame
(garbage copied at the wrong size and stretched) and then died *inside the
driver*, which is why the SEH wrapper never logged anything.

Adding a gate that requires a real colour attachment stopped that specific
crash but the pass **still crashed in game**, so there is at least one more
cause. Candidates not yet eliminated: the viewport not matching the attachment
size, a multisampled attachment, `glPushAttrib` overflowing the attribute stack
mid-frame, or thrashing 84MB texture reallocations as the viewport changes
between passes in a 32-bit address space.

Two lessons worth keeping:

1. **The game multiplexes ONE FBO id across every pass**, swapping attachments.
   "The last bind before fbo=0" says nothing about what is attached. Any hook
   here must inspect the attachments, never assume them.
2. The scene colour is `colorName=7`, a texture at exactly **1680x1050** - the
   window size, bound during *earlier* passes. That, not the fbo=0 transition,
   is where such a pass belongs.

`early_pass` is now **disabled in code**, not merely defaulted off: the value
persists into settings.txt as soon as anyone runs `set early_pass 1`, so a plain
default came back and crashed the game on the next launch.

There is also no performance argument for moving: measured at 60.00 fps vanilla
and 60.00 fps with the swap-time pass (vsync-capped), so the swap-time pass
costs nothing measurable.

## Depth textures are not interchangeable

`depthtex` lists every depth texture with the nearest surface it contains. This
found a serious bug: the "keep whichever FBO was bound last" rule had selected a
6720x4200 buffer whose nearest surface was **999.83 units** - nothing but far
plane, i.e. completely empty. All AO/GI was running against an empty depth
buffer.

Selection now scores candidates: reject all-far-plane buffers, prefer the
largest, and on a size tie prefer the one containing the *closest* geometry
(several buffers share dimensions and only the most complete holds near objects).

```
depthtex          list them with their nearest surface
depthtex <id>     pin one
depthtex auto     re-pick automatically
```

## Open: identifying the ACTIVE camera

The frustum scan finds *a* camera but not reliably the *active* one. Observed
across sessions: `0.5/1000/80deg`, `0.15/1000/90.39deg`, and `0.73/98/11.7deg`.
That last one is an 11.7 degree vertical FOV - a sniper scope, not gameplay.

The game has a third-person and a first-person camera which shift between each
other, plus zoom. Picking "the frustum that repeats most" does not identify
which is currently rendering. Since near/far drive depth linearisation, a wrong
pick reintroduces the original problem. Locating the active camera through the
reflected camera object is the likely fix.

## What is left

- Intensity tuning. With the signal correct, `ao_intensity` / `ssgi_intensity`
  can go far higher than the old values, which were dialled down to hide noise.
- `g_sceneDepthTex` takes whichever FBO depth was bound last. It happens to be
  the scene's, but a size filter (match the backbuffer) would make it robust.
- The 2x supersampled depth is sampled with normalised UVs, so the resolution
  mismatch is harmless - but it does mean depth is higher-resolution than the
  colour we composite against.
