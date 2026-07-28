# Work journal - 2026-07-26

Running log while the user is away. Newest entries at the bottom.

---

## Starting state (when the user left)

**Fixed and verified this morning:**

1. **Black textures** - texture-ID recycling. Our "swallow the vanilla mips"
   list was permanent, but OpenGL reuses texture names across level loads, so a
   stale entry matched an unrelated new texture and ate its mip chain, leaving
   it incomplete → rendered solid black. Now tracks only the most recent
   substitution and clears on any non-substituted level-0 upload.
   Confirmed gone in the general store save.

2. **F10 freeze** - `FIRSTFRAME: uniforms=131781 ms` → **266 ms**.
   Not shader compilation, not the depth pick (15 ms). It was
   `SWSE_ScanCameraFrustum` running *from the render path*: walking ~4 GB of
   address space four bytes at a time, calling `tan()` on candidates. Bounded it
   to the heap (0x10000000-0x40000000) like every other scan, and collapsed the
   two `SWSE_SceneProjection` calls per frame into one.

**Added, unverified:** `near_cutoff` (default 3 world units) excludes the
first-person weapon from AO/SSGI/DoF. Compiles and the gate is live, but I could
not frame the crossbow to confirm it visually.

**Open:** UI bleed-through. `near_cutoff` cannot fix it - UI is drawn after the
depth pass, so UI pixels carry the depth of the world *behind* them, which is far
away. No depth threshold separates UI from that scenery.

## Task while away

Build the pre-UI post-process properly, which fixes the UI bleed AND the weapon
in one go. Then look at the >1080p fullscreen problem (needs launcher changes).

---

## 14:30 - Backed up first

Per request, before touching anything:
- `SWSE_WORKING_RTGI_2026-07-26_1430.zip` (source + tools, 18.8 MB)
- `dinput8_WORKING_2026-07-26_1430.dll` - the exact working DLL, droppable
  straight into `bin\` with no rebuild
- `settings_working_backup.txt` copied into `swse/research/`

## 14:35 - Traced the real in-game frame

The earlier trace was captured at the MENU (3 passes) and was misleading. In
game there are 27:

```
bind  0: vp=1920x1080  color=tex 7
bind  9: vp=7680x4320  color=tex 7
bind 25: vp=3840x2160  color=tex 7     <- last pass with the scene colour
bind 26: vp=7680x4320  color=NONE  -> fbo=0   <- depth-only (the old crash)
```

So the finished scene lives in **texture 7**, and the pass right before `fbo=0`
has no colour attachment at all - which is exactly why the first attempt
crashed copying colour from it.

## 14:45 - Rebuilt the early pass against the right target

Rewrote it to attach texture 7 to an FBO of our own and ping-pong through a
copy, instead of using whatever framebuffer happened to be bound. Added the FBO
entry points (EXT with core fallback), tracked the scene colour in glspy, and
re-enabled the `early_pass` setting.

**It no longer crashes.** That is real progress - the crash is understood and
gone.

## 14:55 - But it is NOT usable. Two blockers.

**1. The frame comes out sheared** into diagonal stripes. Instrumentation found
the cause of the first shear:

```
EARLYPASS: viewport 3840x2160 but texture 7 is 7680x4320
```

The game renders into a 3840x2160 REGION of a 7680x4320 texture. I corrected the
copy/viewport to work in that region - **and it still shears**, so there is at
least one more geometry mismatch I have not found (likely the sampled UV range,
since our copy is region-sized while the attachment is full-sized).

**2. Performance makes it a dead end anyway: 10 fps.**

This is the important finding, and it reframes the whole idea. Swap-time
processing is cheap *because* it works on the 1920x1080 backbuffer. The game's
internal scene target is **7680x4320 - sixteen times the pixel count**. Any
pass that runs there pays 16x the shader cost. Measured:

| where | fps |
|---|---|
| swap-time (1920x1080) | **48** |
| early pass (supersampled target) | **10** |

Even with the shear fixed, 10 fps is unusable. **Moving the post-process before
UI compositing is not viable at this engine's internal resolution.**

## Conclusion on the UI bleed

The pre-UI approach should be considered closed unless the shader is made
dramatically cheaper. Remaining options, in rough order of promise:

1. **A UI mask.** Capture the scene colour before UI (cheap - a GPU copy, no
   shader), then at swap time compare against the final frame; pixels that
   changed are UI, and are excluded from the effect. Costs one texture copy per
   frame rather than 16x the shading.
2. **Accept it.** The bleed only shows on the inventory/menu screens, which are
   paused anyway.
3. Reduce shader cost enough for the early pass to be affordable - a big
   rewrite, and it would still be 16x the pixels.

Option 1 is the one worth trying next.

## State left for the user

- `early_pass 0` in code default AND in settings.txt - it cannot come back on
- RTGI on, **48 fps**, black-texture and freeze fixes both live
- The early-pass code is committed to the working tree but inert behind the flag
- No game files harmed; backups listed above

## 15:20 - Weapon self-occlusion FIXED (user confirmed)

Symptom: the crossbow's critters and upper body cast shadow onto the lower bow,
and the weapon smeared a dark patch on the ground. Two wrong attempts first,
worth recording so they are not repeated:

1. **`near_cutoff` as an exclusion** - skip the effect for pixels closer than N
   units. Wrong: it stopped the critters on the bow being lit at all, and NPCs
   walking toward the player visibly POPPED out of the effect at the boundary.
   Rejected by the user, correctly.
2. **Tightening the gather radius alone** (`ssgi_maxscreen`, replacing a
   hardcoded 0.45 cap that let near pixels gather from 45% of the screen).
   A genuine bug and worth keeping, but it did not fix the self-shadowing.

**What worked: make the rule asymmetric.** The requirement was never "exclude
near geometry" - it was:

* near geometry may be **LIT** (critters, approaching chickens keep lighting)
* near geometry may not **OCCLUDE** (weapon stops shadowing itself and the ground)

So `near_cutoff` now rejects samples whose *hit surface* is nearer than the
cutoff, in both the AO ring and the SSGI ray march, instead of gating whole
pixels out of the effect. Near objects are still shaded; they simply cannot act
as occluders.

Settled values (in code defaults AND settings.txt):

```
near_cutoff     2      # closer than this may be lit, may not occlude
ssgi_maxscreen  0.12   # was hardcoded 0.45
```

48 fps. User confirmed fixed.

## Not started

The >1080p / 4K fullscreen problem. Useful fact discovered along the way:
the window is 1920x1080 while the game renders internally at 7680x4320, so the
supersample factor is **4x per axis**. If that factor is fixed rather than
adaptive, a 4K window would imply a 15360x8640 internal target - which would
plausibly fail to allocate. That is a strong lead for the fullscreen bug and
should be checked first.

