# SWSE - Stranger's Wrath Script Extender

Everything SWSE can do today. The SWSE DLL is the runtime half of the project:
a `dinput8.dll` proxy loaded by the game, giving an in-game console, graphics
features, and the reverse-engineering instruments the rest of the project is
built with. The offline half (archive parsing, texture pipeline, the Mod
Loader GUI) is the `oddforge` Python library and `tools/`.

**Install:** `swse/build.bat`, then copy `dinput8.dll` into the game's `bin\`.
The real system `dinput8.dll` must sit beside it as `dinput8_real.dll`.

---

## Feature switches

Every SWSE system can be switched off individually in `SWSEMods\features.txt`:

```
console    = on      # in-game console + remote mailbox
graphics   = on      # post-process pipeline (RTGI etc.)
hdtextures = on      # .oft texture replacement at upload
hitreact   = on      # additive hit reactions
foliage    = on      # foliage identification + wind
aituning   = on      # aiprefs.txt NPC tuning
```

A disabled feature installs no hooks at all, so the game runs as if SWSE never
touched that system. The file is read once at startup; a missing file means
everything on. `features` in the console shows the current state, and the
self-test reports a disabled system as a warning, never a failure.

This is the right first stop when debugging: turning everything off except the
system you are working on removes every other variable.

---

## Driving the console

Two ways in:

* **In game** - the `` ` `` / `~` key opens the console. Tab completes,
  PageUp/PageDown scroll, Enter runs.
* **From outside** - `powershell -File tools/swse.ps1 "<command>"`. This writes
  a sequence-numbered request into
  `SWSEMods\SWSE Console\remote_in.txt` and reads the reply back from
  `remote_out.txt`. It needs **no window focus**, which matters: the console
  reads the keyboard with `GetAsyncKeyState`, so synthesising keystrokes would
  type into whatever window is actually focused.

`help` lists everything by category; `help <category>` or `help <command>`
narrows it.

---

## Graphics

### RTGI / screen-space global illumination

A real post-process pass injected at `wglSwapBuffers`, using the game's own
scene depth and colour buffers.

| Command | What it does |
|---|---|
| `gfx on\|off\|toggle\|reload` | enable the post-process pass |
| `set <key> <value>` | live-tune any setting |
| `depthtex [id\|auto]` | list / pin the scene depth texture |
| `proj [scan]` | the engine's real near / far / FOV |
| `fbotrace [n]` | log the frame's FBO bind order |
| `framerate` | fps since the previous call |

Settings live in `SWSEMods\SWSE Graphics\settings.txt`.

Finding the right depth buffer was the hard part: the picker rejects buffers
over 12M pixels on purpose, because the supersampled buffer looks plausible and
breaks GI everywhere. See [ROADMAP.md](ROADMAP.md) for the outstanding
first-person weapon transparency issue.

### HD texture replacement

Textures are swapped at GPU upload - **archives are never modified**. Each
vanilla texture is fingerprinted as
`fnv1a32(level0[:4096]) ^ w*73856093 ^ h*19349663`, and a replacement is loaded
from `SWSEMods\SWSE HD\textures\<FINGERPRINT>.oft` if one exists.

Currently **960 textures live**, out of **1,119 replaceable** in the game. The
rest are deliberate exclusions: UI, menus, `.swf` art, normal maps,
particle/NPC effects and sky, which either upscale badly or are not surfaces.
Because substitution happens at upload, a bad texture just looks wrong -
delete the file and it is gone.

`hd` reports installed / substituted / **failed**. The failure count matters
because a replacement that fails to load silently falls back to vanilla: it
looks completely normal in play, so nothing but a counter will ever tell you.

#### Transparent textures - the trap

**Do not pack an alpha-cutout texture without its alpha.** The engine uploads
these as `GL_COMPRESSED_RGBA_S3TC_DXT1` (DXT1 *with* 1-bit alpha), but the
export/pack path works in RGB, so a texture that relied on transparency comes
back fully opaque and every formerly-clear pixel renders solid black. On a
tree that means the whole branch card appears as a black shard - the canopy
looks like shattered geometry, which sends you hunting a vertex-program bug
that does not exist.

Names ending `_AT` (alpha test) are the obvious cases, but the check is exact
rather than name-based: a DXT1 block with `color0 <= color1` is in 3-colour
mode where index 3 means transparent.

```
python tools/alpha_check.py               # report
python tools/alpha_check.py --quarantine  # move offenders aside -> vanilla
```

Run it after **every** batch, with the game closed - a running game holds the
archives locked, and the tool then skips them and still prints a total.

**And read the "located N of M" line.** `alpha_check` once reported zero
offenders while never having looked at 718 of 981 installed files, because its
manifest list was hardcoded and the new batch lived in a new folder. That is
how 13 broken trees shipped. It now discovers manifests and warns loudly when
any installed file is unaccounted for, because *a check that cannot see the
files reads exactly like a pass*.

**The pipeline**, end to end:

```
hd_export.py    archives      -> PNG named by fingerprint
upscayl-bin     ultrasharp-4x -> 4x PNG
downscale.py    4x            -> 2x  (Ultrasharp is a native-4x model; 2x is
                                      what Upscayl's own 2x setting does, and
                                      stranger.exe is a 32-bit process)
seam_check.py   before + after-> which textures tile, and whether they still do
seam_fix.py     broken only   -> edge feather to restore the wrap
hd_pack.py      PNG           -> .oft
```

**Seams are measured, not assumed.** AI upscalers process images in padded
tiles and quietly break the edge wrap, which shows up as a grid of seams across
the ground. `seam_check.py` compares the left/right column difference against
the interior column difference; a seamless texture sits near 1.0. Run it on the
originals first to learn which textures were tiling at all - many never do.
In the most recent batch 41 of 609 lost their wrap and were repaired, leaving
**158 of 158 tiling textures verified seamless.**

### AI difficulty profiles

The game's own difficulty menu sends **one integer** and changes no AI
parameter at all (`EASY`/`MEDIUM`/`HARD` = `new_game` 0/1/2, decoded from
`difficulty_menu.swf` - see `research/AI_SYSTEMS.md`). Everything that decides
how hard a fight feels lives in per-character prefs objects difficulty never
reads.

`SWSEMods\SWSE Console\aiprefs.txt` defines named profiles that reach
them. **It ships `active = off` - nothing is tuned unless a mod author turns
it on.** Three worked examples are included to copy from: `keen` (light),
`relentless` (harder), and `obvious` (deliberately absurd, for checking the
tuning is landing at all).

| Command | What it does |
|---|---|
| `difficulty <name>` | apply a profile now |
| `difficulty off` | restore the shipped values exactly |
| `difficulty` | show what is active |
| `aitune` | reload `aiprefs.txt` live |
| `findai [ms]` | locate perception objects, print their sight values |
| `weapons [ms]` | every NPC weapon: fire rate, reload, accuracy |
| `npcguns [ms]` | which character carries which gun, with hp and bounty |

Setting `active = <name>` applies the profile **once per level load** - never
on a timer, and the scan runs on a worker thread so it cannot stall a frame.
It must re-apply per level because a level load builds fresh prefs objects;
without that the tuning silently lapses when you change area.

**Values are multipliers, not absolutes.** Characters differ a lot - one
outlaw sees 50 units, another 115, another through a narrow 70°×110° cone, and
fire rates run 0.1 to 10 shots/second - so a flat number would erase
hand-authored variety.

**Only enemies are tuned.** Weapons are reached *through* their owning
character (`NPCPrefs.m_rangedWeapon` joins to `NPCWeaponPrefs`'s own hash at
`+0x0C`), so anything at 100000 health - townsfolk, Clakkerz, natives - is
skipped by construction rather than by hoping. The player is never touched.

Live-verified perception defaults:

| | normal | agitated | combat | panic |
|---|---|---|---|---|
| `m_seeDistance` | 50 | 100 | 100 | 75 |
| `m_6thSenseDistance` | 10 | 10 | 10 | 10 |
| view cone | 90×90 | 90×90 | 90×90 | 90×90 |
| `m_hideVolSeeDistance` | 1 | 2 | 3 | 3 |

Sight **doubles** the moment an NPC is agitated - the shipped model, measured.

Two properties tested rather than hoped for: every apply is computed from a
stored baseline, so **applying twice does not compound**, and `difficulty off`
lands back on the exact shipped numbers.

**Units matter, and two of them are counter-intuitive.** `m_fireRate` is a
rate in **shots per second** (higher = faster), and `m_accuracyWidth` is a
**spread** (lower = more accurate). Both were documented backwards at first;
the fire-rate error was only caught in play, because the numbers were
self-consistent and wrong.

`misstime` and `decisionrate` parse but are ignored: `m_missTime` measures
10 ms on nearly every character (the engine can make NPCs deliberately miss
and essentially does not), and no NPC think-rate field exists anywhere in the
1910 reflected fields.

### Foliage wind

Grass and plants are static in the vanilla game. SWSE bends them by rewriting
the engine's ARB vertex programs at runtime.

Two effects: a constant gentle sway, and foliage that **parts around the
player** as they walk through it.

| Command | What it does |
|---|---|
| `wind on\|off` | enable the effect |
| `wind <strength> [speed]` | bend per unit of plant height (0.06 = 6%) |
| `wind weight <n>` | stiffness - tall plants stop whipping (soft curve) |
| `wind push <amt> [radius] [maxheight]` | how hard foliage is shoved aside, and how close |
| `wind axis y\|z` | which local axis is up (**measured: z**) |
| `wind seed on\|off` | per-plant phase, so plants do not sway in step |
| `wind gate on\|off` | restrict wind to foliage draws |
| `wind test on\|off` | exaggerate, for diagnosing direction and pivot |
| `wind save` | persist to `wind.txt` |
| `foliage on` | install the bind tracker wind depends on |
| `foliage scan` / `scanned` | capture one frame's textures to a file |
| `foliage progs` | which vertex programs draw foliage |
| `foliage reload` | re-read `foliage.txt` without restarting |

Settings in `SWSEMods\SWSE Wind\wind.txt` - loaded at launch, so the
effect comes back on by itself. Tuned values: strength 0.06, weight 0.85,
push 0.4 over radius 3.5, Z-up, per-plant phase on.

**Which plants move** is data, not code. `foliage.txt` holds texture
fingerprints, 63 of them, and an entry may carry:

| Flag | Effect |
|---|---|
| `nopush` | sways, but is not shoved aside by the player |
| `sway=<0..1>` | scales this entry's sway; `sway=0.10` is a 90% reduction |
| `nowind` | shorthand for `sway=0` - completely still |

The 20 trees and canopies carry `nopush sway=0.10`. Trees need their own scale
because the global `weight` control damps by plant *height*, and no height cut
separates a tree from tall grass - one that stilled the trees also killed the
grass. A canopy that visibly travels reads as rubber rather than wood.

`all_alpha_textures.txt` beside it lists every alpha-cutout texture
in the game with its fingerprint, to copy entries from. Cacti, trunks, terrain
grass and tyres are excluded entirely. After editing, `foliage reload` applies
it immediately, rebuilding the flags for textures already loaded.

It survives level changes: the engine recreates its shader programs on load,
which silently reverts the injection, so SWSE reads its own marker back every
two seconds and re-injects. Measured cost: **no fps change**.

### Shader reconnaissance

`shaderdump [file|stats]` dumps every shader the driver holds - GLSL programs
and ARB assembly programs - to `bin\swse_shaders.txt`, and reports how many
vertex programs use fixed-function matrix state. That number decided how wind
had to be built. Read-only; nothing is patched.

### Screenshots

`snap [file.tga]` captures from inside the engine via `glReadPixels`, so it
works regardless of window focus or overlay state.

---

## Self-test - is everything actually working?

`selftest` checks every feature and reports what it found. It runs
automatically about five seconds after a level becomes ready, and writes to
`bin\swse_selftest.txt` as well as the console.

```
=== SWSE self-test ===
[PASS] frame hook     last frame 16 ms, worst 627 ms, 3 stalls >80ms
[PASS] hit reacts     ON, actors present, 301 polls, 0 hits seen
[PASS] foliage        63 fingerprints, 11 live here, 216 binds/frame
[PASS] wind           ON, 3 programs injected, 0 failed
[PASS] camera         player projects to 683,121 of 1280x720
[PASS] HD textures    981 installed, 213 substituted so far
[PASS] graphics       post-process pipeline ready
[PASS] agentdebug     ON - runs unfocused, desktop usable
=== 8 passed, 0 warned, 0 FAILED ===
```

**Every check asserts on evidence of work done, never on a flag.** This exists
because a change to the actor scan once left hit reactions completely dead
while the feature still reported itself ON: the hook was installed, the flag
was true, and it detected nothing because its actor list was empty. So
"enabled" is not a pass - "enabled, has actors, and is polling them" is. A
subsystem that is on but idle reads as FAIL, because that is precisely the
state that went unnoticed.

`WARN` means genuinely not applicable here (no foliage in this level, no HD
texture encountered yet) and never means "working".

## Additive hit reactions

Shooting a character pushes the bone that was actually hit. The reaction is
**additive**: it rotates bones on top of whatever animation is playing, rather
than replacing it, so the character keeps walking, aiming or reloading while
absorbing the hit.

### The pipeline

1. **Detect the hit.** A health-drop watcher sweeps live actors, reading
   current/max health at `actor+0x78`. A drop means damage; the amount is the
   delta.
2. **Find where.** Bolts are tracked in flight (hook at RVA `0x91180`,
   position at `+0x24`). The nearest bolt within 12 units of the victim in the
   last moment gives the impact point. With no bolt - melee, explosions - it
   falls back to the torso.
3. **Resolve the bone.** The impact point is matched against the live Granny
   pose to find the nearest bone. Resolve rate is 100%, accurate to 0.1-0.7
   units.
4. **Promote to something visible** (see below).
5. **Spread, envelope, accumulate, write once.**

### Intensity: damage → angle

Damage is converted to a **fraction of the victim's max health** before it
reaches the curve:

```
dmgUnit = 100 * damage / maxHealth        (ratio mode, default ON)
scale   = clamp(curveFloor + dmgUnit * curvePerDmg, 0, curveMax)
angle   = strength * scale                 radians at peak
```

| parameter | default | meaning |
|---|---|---|
| `curveFloor` | 0.25 | even a scratch registers |
| `curvePerDmg` | 0.04 | per 1% of max health lost |
| `curveMax` | 0.50 | ceiling, so a killing blow cannot snap the rig |
| `strength` | 1.2 rad | angle at full scale |
| duration | 360 ms | |

**Why the ratio matters.** With an absolute curve, Wolvarks looked immune - a
1.4-damage hit produced 5.4°, while the same weapon on the player read fine.
Health totals differ by orders of magnitude across characters (Clakkerz sit at
100,000), so a percentage makes "a scratch" and "nearly killed me" mean the
same thing on every rig, including ones never tested. `hitreact ratio 0` uses
raw damage instead.

### The envelope: how a reaction moves over time

The original curve was `(1-t)²` - full rotation on the very first frame, then a
glide back. That **popped on** and read as a glitch. A real impact has a fast
but finite onset, overshoots, and settles, so the envelope has two phases:

```
attack   t < 0.18 :  k = smoothstep(t/0.18) * strength      (3u² - 2u³)
settle   otherwise:  u = (t - 0.18) / 0.82
                     k = ((1-u)² - overshoot * sin(pi*u)) * strength
```

`attack` 0.18 (fraction of duration spent rising), `overshoot` 0.12 (how far
past rest it counter-swings). Smoothstep avoids a corner at either end;
`attack 0` restores the instant onset. An expiring reaction runs one final pass
at `k = 0` so nothing is left in the pose.

### Spread along the spine

A hit does not rotate one bone. It spreads **up the spine away from the root**
across `chain` links, each taking `falloff` of the previous - chest full, neck
less, head least. Defaults: **3 links, 0.6 falloff**. Weights are fixed per
link so the unwind cancels exactly.

Chaining the other way, toward the root, reached the pelvis and blew the hips
out, because rotating a bone that low drags the legs with it.

### Limb-mass promotion: why hits used to be invisible

Reactions on outlaws read as "barely noticeable" while the numbers looked
healthy - 19.5° rotations were landing on **fingertips**. A rotation is only
visible if geometry hangs below it.

So the struck bone is promoted toward the root until at least **`limbmass` (14)
descendants** hang below it, capped at 8 steps. The count comes from the
skeleton's subtree sizes, derived at runtime - no per-rig data is hardcoded.

### Concurrency: surviving a burst

Accumulating rather than multiplying stops reactions compounding, but ten
simultaneous hits still **summed** to an absurd angle and tore the pose apart.
When more than one reaction is live on a character, each accumulated
quaternion's **angle** (not its components) is scaled by `1/N`, floored at
**0.45** so the last hits stay readable. Borrowed from the Fallout NV additive
hit reaction mod, which divides blend weight the same way.

### Arm damping

`armdamp` counter-rotates the arm roots by a fraction of what was applied to
their parent, so the torso absorbs the hit while the hands - and the crossbow
they hold - stay pointing where they were. `0` = arms ride the body (default,
signed off in play), `1` = arms hold station.

Detecting arm roots requires **exactly 2 branches** off the chest: a 20-bone
creature rig had four 3-link chains and three were mistaken for arms.

### Commands

| Command | What it does |
|---|---|
| `hitreact on\|off\|test` | enable / fire a test reaction |
| `hitreact <strength> <ms>` | peak angle in radians and duration |
| `hitreact curve <floor> <perDmg> <max>` | the damage→scale curve |
| `hitreact ratio 0\|1` | damage as % of max health, or raw |
| `hitreact ease <attack> <overshoot>` | envelope shape |
| `hitreact chain <links> <falloff>` | spread along the spine |
| `hitreact limbmass <n>` | minimum descendants before a bone is used |
| `hitreact armdamp <0..1>` | how much the arms resist the torso |
| `hitreact save` | persist to `hitreact.txt` |
| `hitreact hits` | recent hits with damage and resulting scale |
| `hitreact bones` | which bones the last reaction actually wrote |
| `hitreact bolts` | tracked projectiles |
| `hitreact freeze` / `tpose` | diagnostics: stop animation / flatten to bind pose |

### It comes up ON

Settings live in `SWSEMods\SWSE Combat\hitreact.txt`, read on the first
frame, which installs the pose hook, the bolt hook and the health watcher and
switches the feature on. With no file the compiled defaults are used and it
still comes up on - those defaults ARE the values signed off in play:

```
enabled 1
strength 1.2000
ms 360
curve 0.2500 0.0400 0.5000
ratio 1
ease 0.1800 0.1200
chain 3 0.6000
limbmass 14
armdamp 0.0000
```

### Safety

`ApplyReactions` wraps its body in `__try` and disables itself on a fault
rather than taking the game down. Turning it off never unpatches the hook (the
render thread may be inside it - that crashed the game once); it just stops
contributing. Cost is about **2 fps**.

---

## Player, movement and items

**Player:** `hp`, `stam`, `sethealth`, `heal`, `maxhealth`, `maxstamina`,
`god`, `kill`, `pfield`

**Movement:** `pos`, `savepos`, `tp`, `up`, `move`, `gravity`, `aircontrol`,
`jump`, `speed`

**Items:** `grant`, `giveartifact`, `allartifacts`, `artifacts` (all 53),
`giveweapon`, `moolah`, `money`, `ammo`, `defaultammo`, `noammo`, `crossbow`,
`noweapons`

**View / state:** `fps` / `nofps` / `sniper`, `steef`, `stranger`, `naked`,
`save`, `checkpoint`, `loadsave`, `healthbars`, `weaponhud`, `tphome`,
`tpreset`

**World:** `warp <level|0-6>`, `levels`

---

## NPCs

The largest command group, and the one with the most reverse-engineering
behind it.

**Spawning / placement:** `spawn`, `spawnhere`, `spawnnpc`, `npcnow`,
`npcdupe`, `npccount`, `dupetype`, `npcreplay`, `npchere`, `npclast`,
`spawnclone`, `npcspawn`, `bring`, `sendnpc`, `spawnradius`, `critters`

**Tuning:** `npchealth`, `npcelite` (promote a fraction to elites), `npcgib`,
`npchurt`, `npcaff`, `allnpcs`, `tuning` (reload `characters.txt`), `types`

**Finding things:** `npcs`, `npctypes`, `npctags`, `spawntypes`, `npcnear`,
`whereis`, `geominst`, `resolve`, `strhash`

**Hostility and alarms:** `townpanic`, `raid`, `raidmode`, `attack`, `decoy`,
`feud`, `findtarget`, `scantargets`

**Known limit:** there is no NPC-vs-NPC hostility in this engine - every
character reads affiliation `1`, and "enemy" is a relationship to the player
only. Faction raids are blocked on this; see [ROADMAP.md](ROADMAP.md).

---

## Scripting

The game ships a script VM, and SWSE can call into it.

| Command | What it does |
|---|---|
| `list [filter]` | 181 discovered game functions |
| `call <function> [args]` | call one |
| `scripts` / `reloadscripts` | your own `.txt` command files |
| `ptr` / `get` / `hold` / `unhold` / `ptrreload` | named pointer chains |

---

## AgentDebugMode

`agentdebug [on|off]` (alias `background`) keeps the game simulating while
alt-tabbed **and** gives up the cursor and keyboard, so the desktop stays
usable. Off by default, and every hook involved is installed but inert, so
there is no behaviour to regress.

The first version only did the first half and trapped the pointer inside the
game window. Real focus is now tracked separately from the focus the engine is
told about.

---

## Extending the AI tuning - technical reference

This section is for anyone who wants to add a new tunable field, or reach a
class the tuning does not touch yet. It is the part that took longest to get
right, and most of the cost was avoidable.

### The engine's reflection system

`stranger.exe` registers **1910 reflected fields** at startup. Each one is a
12-byte descriptor built inline, and - this is the useful part - **the field's
offset is a literal in the instruction stream**:

```
6a 0c                 push 0Ch                 ; sizeof(descriptor)
e8 <rel32>            call operator new
c7 06 <typedesc>      mov  [esi],   <type>
b8 <nameVA>           mov  eax,     <field name string>
c7 46 08 <offset>     mov  [esi+8], <FIELD OFFSET>     <-- exact, not inferred
```

`tools/reflect_dump.py` reads this:

```bash
python tools/reflect_dump.py                      # summary
python tools/reflect_dump.py --field m_fireRate   # locate one field
python tools/reflect_dump.py --csv out.csv        # all 1910
```

**Do not use `research/REFLECTION_SCHEMA.md` for addressing memory.** It says
so in its own header - its class grouping is a heuristic based on name
proximity, and it is wrong in ways that look right. It filed the perception
fields under a class called `CoverDuration`, which has nothing to do with
sight.

**Class boundaries come from offset monotonicity.** A class registers its
fields in ascending offset order, so a *decrease* is a class boundary. Grouping
by code position instead merged several classes into one run and produced runs
with three different fields all claiming offset `0x4`. The monotonic rule took
the dump from 49 merged blobs to 294 clean runs, and it is self-checking: a
correct run comes out strictly ascending.

### Finding a prefs object at runtime

The obvious route does not work. **`ResolvePrefs()` is NPCPrefs-specific** -
handed a weapon or AI-prefs hash it returns an unrelated *character* record
instead of failing, so the offsets get applied to the wrong object and read as
plausible garbage (direction vectors, `0.707` = cos 45°).

Two routes that do work, in order of preference:

**1. By vtable, recovered from RTTI.** This is real identification and is safe
to write through.

```
.?AVNPCWeaponPrefs@@   ->  vtable 0x776454   (RVA 0x376454)
.?AVNPCPrefs@@         ->  vtable 0x767E1C   (RVA 0x367E1C)
```

The walk is: find the type descriptor (the mangled name string, minus 8 bytes),
find the `RTTICompleteObjectLocator` that points at it, then find the vtable
whose `[-1]` slot points at that locator. `stranger.exe` is ASLR'd, so rebase:
`GetModuleHandle(NULL) + RVA`. **The method self-checks** - it returns
`NPCPrefs = 0x367E1C`, matching the value hardcoded in `PrefsOfNpc` from
entirely separate work.

**2. By memory shape, when no vtable is available.** Only acceptable with a
*structural* signature, not a plausibility test. The perception object has four
sight blocks at a fixed `0xA4` stride, and across those four states only two
fields ever change:

```
6th sense    10   10   10   10      invariant
seeAbove   1001 1001 1001 1001      invariant, and == seeBelow
h/v angle    90   90   90   90      invariant
instant      10   10   10   10      invariant
------------------------------------------------
seeDist      50  100  100   75      varies   <- the model
hideVol       1    2    3    3      varies   <- the model
```

Requiring all six invariants across all four blocks is a demanding test that
unrelated memory does not reproduce.

> **This is the mistake to learn from.** The first version matched only "four
> blocks of plausible floats". A window of believable floats stays believable
> when shifted four bytes, so it matched overlapping slides of the same object
> *and* unrelated objects. Writing through those corrupted the game - a
> flashing character model, and once a hard crash. The tell was there and
> ignored: the hit count jumped from 7 to 19 between runs. **If a scan's count
> is unstable, stop and fix identification before writing anything.**

### Joining a character to its equipment

Both classes carry their own path hash at `+0x0C`, which gives a clean join:

```
NPCPrefs +0x498  m_rangedWeapon   ==   NPCWeaponPrefs +0x0C  (own hash)
```

`SWSE_NpcGuns()` performs it. This is what makes tuning **selective** - a bare
scan for weapon objects has no idea whose gun it is, but the join gives each
weapon's owner, so the protected cast (100000 health) can be excluded. The
unset sentinel for any hash-valued field is **`0x2DFD1072`**; it does not
resolve and must be special-cased.

### The apply model - baselines, not increments

`aitune.cpp` stores the **shipped value** the first time it touches an object
and computes every write from that baseline:

```c
value = baseline * multiplier;      // never  value = value * multiplier
```

Two properties fall out, and both are worth preserving in any extension:
applying a profile twice does not compound, and restoring lands on the exact
original numbers rather than on `value / multiplier` rounding drift.

A shipped value of `0` means the field is unused for that character. Skip it -
multiplying it writes `0` and looks like it worked.

### Adding a new tunable field

1. `python tools/reflect_dump.py --field m_yourField` - get the exact offset
   and see which run (class) it belongs to.
2. Confirm the class has a recoverable vtable; if not, find a structural
   signature with invariants, not a range test.
3. Read it live first - `peek <addr>` - and sanity-check the values against
   what you see in play. **Units are not obvious.** `m_fireRate` is shots per
   second, not a delay. `m_accuracyWidth` is a spread, so lower is better.
   Both were documented backwards here at first.
4. Add the field to `WBase`/`Baseline`, capture it, and scale from the
   baseline.
5. Add the key to `FieldOf()` and document it in `aiprefs.txt`.

### Things that do not exist

Worth knowing before spending time looking:

* **No NPC think-rate field.** Nothing for decision cadence anywhere in the
  1910 fields. `m_checkEverySeconds` belongs to a level-scripting trigger (it
  sits with `m_restrictToGuyType`, `m_restrictToBrainState`,
  `m_dependOnGlobalVar`), not the NPC brain.
* **No per-character animation-rate field.** The only `m_animSpeedLo/Hi`
  belongs to the ambient critter class.
* **`m_missTime` is inert in practice** - 10 ms on nearly every character. The
  engine models deliberate missing and essentially does not use it.
* **Difficulty touches no AI parameter.** The menu sends one integer
  (`new_game` 0/1/2, decoded from `difficulty_menu.swf`) and there is no
  difficulty field in the reflection schema at all.

## Reverse-engineering instruments

These exist because nearly every wrong turn in this project came from reasoning
about what must be true, and nearly every advance came from measuring it.

**Memory:** `peek`, `dumpaddr`, `probe`, `invdump`, `find`, `findval`,
`narrow`, `poke`, `freeze`, `unfreeze`, `unfreezeall`, `anchor`

**Watchpoints (hardware DR0-DR7):** `watchaddr` (what writes here?),
`watchrw` (what reads this?), `watchexec <rva> [once]` (is this code reached?),
`watchoff`, `watchinv`

**Objects and classes:** `whatis` (RTTI), `instances`, `nearby`, `vtscan`,
`ctxinfo`, `vcall`, `diff`, `difftypes`

**Animation:** `granny` (find bone poses), `anim`

**Tracing:** `spy`, `grantspy`, `grantlast`, `npcspy`, `npchits`, `watch`

---

## Offline tooling (`tools/`, `oddforge/`)

| Tool | What it does |
|---|---|
| `oddforge/` | SMB container + TOC parser, byte-identical round-trip on all 1,222 archives |
| `tools/texmap.py` | map every texture to its runtime fingerprint; `--fp-in` names what is on screen, `--match/--fp-out` builds fingerprint lists |
| `tools/census_textures.py` | how many textures exist, by format and size |
| `tools/hd_export.py`, `hd_pack.py` | the HD texture pipeline |
| `tools/downscale.py`, `seam_check.py`, `seam_diff.py`, `seam_fix.py` | 4x→2x, and proving the textures still tile |
| `tools/shot.ps1` | capture the game window, to verify visual changes directly |
| `tools/seam_check.py`, `seam_diff.py`, `seam_fix.py` | detect and repair tiling seams broken by AI upscaling |
| `tools/alpha_check.py` | find DXT1 1-bit-alpha cutouts |
| `tools/swse.ps1` | drive the in-game console from the command line |
| `tools/relaunch.ps1` | restart via the launcher, click PLAY, load the last save |
| `tools/laa_patcher.py` | large-address-aware patch |

**The game must be started through `Launcher.exe`** - running `bin\stranger.exe`
directly produces a frozen window.

---

## Files SWSE reads and writes

```
<game>\bin\
  dinput8.dll               SWSE itself
  dinput8_real.dll          the real system DLL
  swse_log.txt              runtime log
  swse_shaders.txt          shaderdump output
  swse_frame_textures.txt   foliage scan output
  swse_snap.tga             screenshots

<game>\SWSEMods\
  SWSE Graphics\settings.txt        RTGI tuning
  SWSE HD\textures\<HASH>.oft       HD texture replacements
  SWSE Wind\wind.txt                wind settings (loaded at launch)
  SWSE Wind\foliage.txt             which textures are foliage
  SWSE Wind\all_alpha_textures.txt  palette to copy entries from
  SWSE Combat\hitreact.txt          hit reactions (loaded at launch)
  SWSE Console\remote_in.txt        command mailbox
  SWSE Console\remote_out.txt       replies
```

---

## Research notes

Deeper write-ups live in `swse/research/`:
`FOLIAGE_WIND.md`, `ANIMATION.md`, `NPC_TUNING.md`, `NPC_SPAWNING.md`,
`GRAPHICS_RTGI.md`, `HD_TEXTURES.md`, `ALL_FUNCTIONS.md`.
