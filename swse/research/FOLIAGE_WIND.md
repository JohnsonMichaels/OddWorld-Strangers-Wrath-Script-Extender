# Foliage wind - making static grass and plants move

Goal: grass and foliage in this game never move. Give them a gentle, constant
life-like sway, toggleable. Later: plants near the player push aside as if
displaced by the player's movement.

## The decisive measurement (2026-07-27)

Everything about how this must be built depends on ONE fact: how the engine
transforms vertices. Measured with `shaderdump`, which enumerates every shader
object the driver holds and reads its source back. Read-only - nothing patched.

```
GL_VERSION : 4.6.0 Compatibility Profile Context   (AMD Radeon RX 9060 XT)

GLSL programs                 1     <- that one is OURS (the RTGI post-process)
ARB vertex programs         137
ARB fragment programs        94
references to state.matrix    0
references to program.env     0
highest program.local used  179
vertex.attrib references   1488     (matrix-palette skinning)
ARL address-register uses    269
```

The engine is **entirely ARB assembly shaders**, Cg-compiled:

```
!!ARBvp1.0
# cgc version 3.1.0010, build date Feb  8 2012
# profile arbvp1 ... NumTemps=32 MaxInstructions=4096 MaxLocalParams=256
PARAM c[2] = { program.local[0..1] };
```

### What this RULES OUT

**The modelview-shear trick does not work here.** The cheap way to bend foliage
is to multiply the modelview by an object-space shear (`x' = x + k*y`) before
the draw - zero displacement at the base, maximum at the top, no shader work at
all. That requires vertex programs to transform through fixed-function matrix
state.

`state.matrix` appears **zero** times in 137 vertex programs. The engine passes
its own MVP in `program.local` parameters, so `glMultMatrixf` on the modelview
would be ignored completely. Had this not been checked first, the shear would
have been built, would have done nothing, and the silence would have been blamed
on the wind maths rather than on the mechanism.

### What this ENABLES

* **`program.env` is completely unused by the game** - 0 references across all
  231 programs. That is a free, GLOBAL parameter space: one
  `glProgramEnvParameter4fARB` call reaches every bound vertex program, so the
  wind vector needs no per-program bookkeeping.
* Instruction budget is 4096 with 32 temps; a displacement costs ~3
  instructions. Cost is irrelevant.
* Locals are used up to index 179, so the engine's own parameter space must be
  left alone - another reason to use `env`.

## Mechanism

Hook `glProgramStringARB` and rewrite foliage vertex programs as they are
uploaded:

1. declare `PARAM swse_wind = program.env[<free index>];`
2. declare `TEMP swseP;`, seed it with `MOV swseP, vertex.position;`
3. displace it, then rewrite every later `vertex.position` reference to `swseP`

Because the displacement is driven by an env parameter, **the injected code is
inert while that parameter is zero**. So every vertex program can be patched
safely and the effect gated per-draw by setting the parameter only when foliage
is being drawn. Nothing else in the scene can move even if identification is
imperfect - a much safer failure mode than patching a guessed subset.

## What drives "height up the plant" - RESOLVED: local Z

Displacement must be zero at the plant's base and greatest at its top,
otherwise plants slide bodily through the ground.

Reading the foliage programs settled the harder half. They open by
dequantising a packed position, and programs 3/4 then transform it by a matrix
held in VERTEX ATTRIBUTES - per-plant instancing:

```
MUL R0, vertex.attrib[0], c[5];    # * scale
ADD R1, R0, c[8];                  # + bias
DP4 R0.x, R1, vertex.attrib[1];    # instance matrix
```

So `R1` is that plant's OWN local space, and the instance matrix does the world
placement. Local height is therefore height above *that plant's* base, not
world height - the failure mode feared above does not arise, and the texcoord
fallback was not needed.

**Which local axis is up could not be read from the shader at all.** The
programs only reveal the projection convention. It was settled by trying both
and looking: with Y the plants sheared and appeared to twist; with **Z** they
hinge at the base and lean over. `wind axis y|z` still exists to re-check.

## Implementation

Two injection points, because the two effects live in different spaces.

**1. Wind - local space**, immediately after the bias add. A triangle wave
(ARBvp1.0 has no SIN: `FRC` gives a sawtooth, `|2u-1|` folds it into a
triangle) drives one sway scalar along a single fixed direction, scaled by
local height.

Driving x and z from *independent* oscillators was the first attempt and was
wrong - the tips traced an ellipse, which reads as plants rotating rather than
blowing.

**2. Player push - world space**, after the instance transform. "Away from the
player" cannot be expressed in a plant's local axes without inverting its
instance transform, so the push is applied where both the position and the
direction are unambiguous. World up is Y here, so the horizontal plane is XZ.

Parameters ride in `program.env`, which the game never uses:

```
env[0] = (windX, 1/weight, windZ, phase)
env[1] = (playerX, playerY, playerZ, 1/radius^2)
env[2] = (pushStrength, 1/pushMax, 0, 0)
```

### Weight - soft saturation, not a clamp

Displacement is `sway * height`, so it grows linearly with plant size: grass
barely moves while a tree swings absurdly. The first fix clamped height with
`MIN`, which damps EVERYTHING above the cap equally - mid-sized plants lost
their motion along with the trees.

The curve `h / (1 + h/cap)` fixes that: small plants stay nearly linear, tall
ones asymptote toward the cap.

| plant height | hard clamp | soft curve |
|---|---|---|
| 0.1 grass | 0.10 | 0.083 |
| 0.5 bush | 0.50 | 0.25 |
| 2 small tree | 0.50 | 0.40 |
| 10 big tree | 0.50 | 0.476 |

### Trees are exempted BY NAME, not by size

Trees must not be shoved aside by the player. A height threshold was tried
first and **cannot work**: `pushmax 1.0` killed the push on grass entirely
while trees still moved, which proves local plant heights do not scale
consistently between types. No single size cut separates them.

So `foliage.txt` carries a per-entry `nopush` flag, and 20 tree and canopy
textures are marked. They sway; they are not pushed. A name-based rule cannot
drift the way a geometric threshold does.

## Failures worth remembering

* **Reading `result.position`.** Programs that transform straight into
  `result.position` rejected the push with `GL_INVALID_OPERATION` - result
  registers are WRITE-ONLY in ARB assembly. Those programs now get wind only.
* **Anchoring on one instruction shape.** The first version keyed on the
  literal `MUL R0, vertex.attrib[0], c[` and matched 3 of 213 programs; Cg's
  `-O3` sometimes fuses scale and bias into one `MAD` and does not always use
  `R0`.
* **Retrying failures forever.** The per-frame catch-up re-attempted failed
  programs every frame, reporting "210 failed" for 3 programs.
* **Enabling before anything is drawn.** Loading from `wind.txt` runs on the
  first frame, when no plant has been drawn and no program is discoverable, so
  gating "on" behind injection success left wind permanently off at launch.
* **One-shot program discovery.** A single scan frame only finds programs for
  plants on screen at that instant, which is why dandelions and ferns stayed
  frozen. Discovery now runs continuously and injects new programs mid-play.
* **Level loads revert everything.** The engine recreates its ARB programs, so
  injections silently disappear. Detected by reading our marker back every two
  seconds rather than by hooking level load; the saved originals then belong to
  destroyed programs and must be dropped, never written back.

## Tuned values (signed off in play)

```
strength   0.060     bend per unit of plant height
weight     0.850     soft-saturation cap
push       0.400     player shove, world units
pushradius 3.500
axis       z
seed       1         per-plant phase
gate       1         foliage draws only
```

Cost: **no measurable fps change** (60.02 vs 60.00, vsync-capped). The bend is
~11 instructions per vertex against a 4096 budget, injection happens once per
program, and the only recurring CPU work is the two-second marker check.

## Identifying foliage - DONE, and the first attempt was wrong

Final state: 47 fingerprints, 11 of them live in the test level, **256 foliage
binds per frame**. The first attempt scored 1.

### How the first attempt failed

A foliage list was written by guessing plant words - tree, fir, canopy, leaf,
bamboo, vine - against archive names. It produced 16 textures and detected
almost nothing, because the test scene is a DESERT: milkweed puffballs, dead
bushes, juniper shrubs, agave, red grass. Not one of those words was in the
guess list.

Worse, the correct names were **invisible to the tool**: `texmap.py` only
fingerprinted DXT1, and every one of those plants is DXT5 (`.tga`, alpha
cutouts). They could not have been found by improving the word list alone.

### What fixed it

`foliage scan` captures every texture bound during ONE frame and writes their
fingerprints; `texmap --fp-in` turns those back into artist names. That answers
"what is actually on screen" instead of "what might a plant be called".

That required fingerprinting non-DXT1 formats. Block size was inferred as 16
bytes per 4x4 block for formats 14/15 (DXT3/DXT5) - and the inference is
**self-checking**: a wrong size produces fingerprints that match nothing the
game uploaded. Identification went 48 -> 64 of 93, which is the confirmation.

### The selection rule

Not a word list - a property. **Foliage cards are `.tga`; ground and structure
are `.bmp`.** Every real plant card in this game is an alpha cutout, while
`brown_dirt_grass_01.bmp`, `green_grass_01.bmp` and `tree_stand_01.bmp` are
terrain and trunks that must NOT move. Requiring `.tga` plus a plant word, and
excluding trunks (`treetrunk`, `tree_stand`), rigid cacti and particles
(`falling_leaf`, `_seed`), gives 47.

20 of the 93 textures in a frame have no fingerprint at all: they are uploaded
through `glTexImage2D` (uncompressed), which SWSE does not hook. None of them
are foliage, so this is recorded rather than fixed.

## Identifying foliage

`glspy` already fingerprints every texture at upload
(`fnv1a32(level0[:4096]) ^ w*73856093 ^ h*19349663`). `tools/texmap.py`
computes the same fingerprint from the archives and knows each texture's NAME,
so a foliage fingerprint list can be generated offline and read at startup.

The `_AT` suffix marks alpha-tested cutouts (`bamboo_leaves_AT.tga`,
`cedar_fir_tree_01_AT.tga`) - i.e. exactly the foliage cards, named as such by
the original artists.

## Tooling added

* `shaderdump [file|stats]` - dump every GLSL and ARB program to
  `bin\swse_shaders.txt`. Scans the id space (there is no "list all programs"
  call), saves and restores the ARB binding, and runs inside `__try`.
* `tools/swse.ps1` - drive the in-game console from the command line through
  the existing `remote_in.txt` mailbox. The console reads the keyboard with
  `GetAsyncKeyState`, so synthesising keystrokes would type into whatever
  window actually has focus; the mailbox needs no focus at all.
