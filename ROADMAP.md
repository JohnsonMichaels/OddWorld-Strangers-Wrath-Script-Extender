# SWSE Roadmap

The goal: a complete modding platform for Oddworld: Stranger's Wrath HD that lets the
community create, share, and install mods - no reverse-engineering knowledge required.

## Architecture (bottom to top)

```
+---------------------------------------------------------+
|  4. Mod Loader (GUI)                               |
|     Windowed editor: NPC stats, weapons, quests,        |
|     locations, textures. "New Mod" -> edit -> save.     |
+---------------------------------------------------------+
|  3. Mod system                                          |
|     MODS/ folder; each mod = manifest + patches.        |
|     Loader merges mods, rebuilds archives, backs up     |
|     originals, one-click enable/disable.                |
+---------------------------------------------------------+
|  2. bounty CLI                                          |
|     catch (extract) / cashin (repack) / diff / verify   |
+---------------------------------------------------------+
|  1. oddforge format library (Python)                    |
|     SMB container + TOC node graph + .smh scripts       |
|     parse AND write, byte-identical round-trip          |
+---------------------------------------------------------+
```

## Design principles

- **Mods are patches, not archives.** A mod folder contains *edits* (changed script
  text, replaced textures, changed values) plus a `mod.json` manifest - never full
  game archives. Keeps mods small, legally clean (no game assets distributed), and
  lets multiple mods coexist by merging patches before rebuild.
- **Originals are sacred.** The loader backs up any archive before touching it and
  can restore a vanilla install in one command.
- **Byte-identical or bust.** The repacker is only trusted once `catch` + `cashin`
  with zero edits reproduces the original file exactly, for all 1,222 archives.

## Milestones

| # | Milestone | Status |
|---|---|---|
| 0 | Container round-trip: parse + rebuild byte-identical (TOC opaque) | DONE 1222/1222 |
| 1 | TOC node-graph parser: account for every byte in small archives | in progress |
| 2 | Extractor (`bounty catch`): textures, scripts, geometry out as files | partial (DXT1 textures) |
| 3 | Repacker (`bounty cashin`): byte-identical rebuild, then modified rebuild | DONE - modified rebuild loads |
| 4 | **First mod loads in game** - modded loading screen confirmed in-game | **DONE 2026-07-23** |
| 5 | `.smh` blockmap format (0xBEEF2B16): level scripts extract/inject | |
| 6 | Mod system: MODS/ folder, manifest, merge, backup/restore | |
| 7 | Mod Loader GUI (NPCs, weapons, quests, textures) | |
| 8 | Community release: docs, GitHub, example mods | |

Milestone 4 was hit the same day the project started: the game accepts rebuilt
archives with no further validation (no checksums over section data). The loader
trusts the container - which means everything else on this roadmap is buildable.

---

## Work to do

### HD textures - alpha cutouts (BLOCKED, 14 textures quarantined)

Tree foliage and similar cutouts rely on DXT1's 1-bit alpha and are held back in
`hd_textures/env/needs_alpha`. Packing them today would put solid black
rectangles in the world where the transparent parts should be.

The pipeline loses alpha in BOTH directions, confirmed in `oddforge/dxt.py`:

* `decode_dxt1` (line ~48) - in 3-colour mode (`c0 <= c1`) index 3 means
  TRANSPARENT, but it writes `(0,0,0)`, i.e. solid black.
* `encode_dxt1` (line ~96) - `img.convert("RGB")` discards alpha and always
  emits opaque 4-colour blocks.

Plan, in order:

1. **Decode to RGBA.** Index 3 in 3-colour mode becomes alpha 0, not black.
2. **Alpha-aware encode.** A 4x4 block containing transparent pixels must emit
   `c0 <= c1` and assign index 3 to those pixels. This is the real work: the
   encoder currently only knows the 4-colour opaque path.
3. **Re-threshold after upscaling.** AI upscalers produce SOFT alpha; DXT1 is
   1-bit. Without a hard cut (~128) the foliage edges come back chewed and
   sparkly. Easy to forget, very visible.
4. **Verify with `tools/alpha_check.py`.** It already detects this exactly, by
   inspecting whether blocks use 3-colour mode with index 3 - not by guessing
   from filenames.

Low risk: nothing touches archives. SWSE substitutes at GPU upload, so a bad
texture looks wrong and you delete the file.

### NPC factions / raids (IN PROGRESS, pending)

Wild NPC populations and faction behaviour - wild slegs in the desert, and
scrabs later if the `.geo` import path is cracked.

Known so far:

* `npcaff <type> [n]` sets affiliation (who fights whom); affiliation lives on
  the LIVE NPC, not in prefs - every character in a town reads affiliation 1
  yet outlaws attack and townsfolk flee.
* The engine never creates NPCs at runtime: all exist from level load, and
  encounters walk pre-placed ones out of hiding. `bring` relocates them.
* `characters.txt` is applied at level load (per-type health / gib /
  hurtReaction), which is the right place for anything that must survive a
  reload.
* `npcelite <type|*> <pct> [hp]` promotes a FRACTION of live NPCs (health is
  per-actor; `m_hurtReaction` is per-type at prefs+0x484 and cannot vary per
  individual).
* Sequencing note: wild slegs need NO import work - slegs already ship in
  Stranger's Wrath. Prove the encounter design with those first; the creature
  swap is a separate problem.

**THE BLOCKER: there is no NPC-vs-NPC hostility.** Established by observation
and by data in [NPC_TUNING.md](swse/research/NPC_TUNING.md):

* `m_affGenerally` reads `1` for EVERY character - townsfolk, outlaws, slogs,
  bosses alike, and `m_affList` (+0x4EC) is 0 on all of them. There is one
  affiliation, so nothing can be on a different side.
* Retyping a third of Buzzarton into outlaw cutters put ~100 armed hostiles in
  a town of townsfolk. They ignored each other completely - no aggression, no
  fleeing, no alarm.
* "Enemy" and "passive" are relationships *to the player*, not to each other.

So a raid cannot be produced by editing prefs or affiliations - there is
nothing to point. It needs the AI driven from outside, which is blocked on the
actor-binding problem in [NPC_SPAWNING.md](swse/research/NPC_SPAWNING.md): a
verb acts on its context's actor, and VM instances are per-running-script
rather than per-actor.

**Targeting / witness system.** Killing an outlaw in front of townsfolk makes
them attack THE PLAYER - they register violence but attribute it to you. There
is a witness system and its only suspect is Stranger. Any raid design has to
account for this, or the player gets blamed for a fight they did not start.

**Alarms (Clakkerz).** Townsfolk raise alarms, and this is reachable:
`PostAlarm` at `0x54CE10`, wrapped as `SWSE_PostAlarm(zoneId, useBell, ...)`.
`AVEventPrefs` also exposes `m_eTownBell` (+0x150), `m_eNPCShout` (+0x1C8) and
`m_ePanic` (+0x1B4). Worth checking whether an alarm can be posted for a zone
WITHOUT the player being the cause - if so it may be a lever for scripted raid
behaviour that affiliation cannot provide.

### RTGI - first-person weapon transparency (LOW PRIORITY, deferred)

The crossbow reads as slightly transparent, with ground shadows visibly running
under it.

**Cause - INTERMITTENT, and it depends on camera state.** The artifact appears
only when the first-person weapon is absent from the scene depth buffer. When
it is absent, the depth at those pixels describes the GROUND BEHIND the weapon
and the GI pass shades them as ground.

Measured across a game restart, same save, same texture set:

| | artifact present | artifact gone |
|---|---|---|
| camera near | 0.500 | 0.150 |
| fov | 80 deg | 106 deg |
| tex 5 nearest | 3.32 (no weapon) | 0.99 (weapon present) |

So this is NOT a depth-texture selection bug - the picker chose tex 5
(3840x2160, the backbuffer-sized scene depth) in both cases and was correct
both times. What changed is whether the engine rendered the weapon into that
buffer at all, which tracks the camera near plane / FOV state.

Diagnosis first, every time: run `depthtex` and read the nearest value for the
buffer in use. Above ~3 units the weapon is not in it and the artifact is
expected; near ~1 it is, and the GI will be correct.

**Why `near_cutoff` cannot fix it.** That setting rejects near OCCLUDERS, which
is why it fixed self-shadowing. This is about what is being SHADED. Tried
raising 2 -> 4: no effect on the artifact, and it risks flattening legitimate
contact shadows.

**Do not "fix" it by changing the depth texture.** Pinning tex 489
(7680x4320, the supersampled buffer) appeared to fix the crossbow and broke GI
everywhere else. `SWSE_AutoPickDepthTex` rejects anything over 12M pixels ON
PURPOSE - the real scene depth is the backbuffer-sized buffer (tex 5,
3840x2160). The picker was right; the pin was wrong.

**The actual fix** is a weapon mask: the GI pass skips the screen region the
first-person weapon occupies rather than trying to infer it from depth. Same
approach as the deferred UI-bleed problem, and the two should be solved
together.
