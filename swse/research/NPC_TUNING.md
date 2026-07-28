# Character tuning - health, death, and hit reactions

Per-character settings live in **`NPCPrefs`**, one object per character type,
shared by every NPC of that type. Change the prefs and the whole species
changes; change a live NPC and only that individual does.

Resolve a type's prefs from its hash with `resolve <hash>`, or read it off a
live NPC with `npcnear`. `NPCPrefs` stores its own hash at **`+0x0C`**, which is
how any NPC can be identified without knowing its name.

## Fields

| offset | field | notes |
|---|---|---|
| `+0x0C` | (own path hash) | identifies the type |
| `+0x438` | `m_geometry` | model reference (a hash) |
| `+0x448` | `m_health` | the whole story behind "invincible" townsfolk |
| `+0x44C` | (max health) | set alongside `m_health` |
| `+0x460` | `m_onDeathGib` | bool - gib instead of a death animation |
| `+0x461` | `m_allowOnDeathGibFromBolts` | bool - already 1 on everything seen |
| `+0x464` | `m_onGibSpawnNPC` | unset (`2DFD1072`) on every character checked |
| `+0x484` | **`m_hurtReaction`** | `0` = staggers, `2` = unflinchable |
| `+0x4A8` | `m_killMoolah` | payout for a kill |
| `+0x4E8` | `m_affGenerally` | looks like faction/affiliation - untested |
| `+0x4F8` | `m_respondsToDamageDynamite` | one of a `m_respondsTo…` family |
| `+0x4FA` | `m_respondsToImmobilizeSpiderBola` | |
| `+0x508` | `m_onDeathCollectableSpawner` | what it drops |

Live NPCs carry their own health at **`+0x78`** (current / max / base), so
changing prefs alone does not affect anyone already standing in the level.

### Immortality is just a big number

Townsfolk are **not** flagged invulnerable, are not a special class, and do not
bypass the damage system. `m_health` is **100000.0** against 45.0 for an outlaw
cutter - every shot was landing and doing 0.045% of their health. Nothing is
disabled, so lowering the number puts them through the normal damage path.

**22 of the 66 characters** in the game are protected this way - not just the
Clakkerz but the natives ("Grubbs"), farmers, storekeepers, the Vykker Doc.

### Knockback resistance is per-character

`m_hurtReaction` (`+0x484`) governs whether a character can be staggered:

| character | value |
|---|---|
| outlaw cutter | `0` - staggers normally |
| the 10000hp heavy (`F4DC66D8`) | `2` - unflinchable |

Found by diffing the two prefs objects; everything else in that block matched.
There is **no player-side equivalent** - `PlayerPrefs` has no such field, and
player knockback appears to run through `GlobalMotionPrefs`
(`m_knockbackCollisionVelocity` `+0x4C`, `m_knockbackElasticity` `+0x50`), which
is global and would flatten everyone's knockback rather than just the player's.

## The tuning file

`SWSEMods\SWSE Console\characters.txt`, read at DLL startup and applied
from inside the spawn hook as each level builds its cast - so it is a mod, not
commands typed after every load. `tuning` reloads it without restarting.

```
#   <hash>    <health>  <gib>  <hurtReaction>      ("-" = leave stock)
5CEE67FD      100       1      -        # townsfolk: mortal and they gib
072A64D6      45        0      2        # cutters that cannot be staggered
*             -         -      -        # everything else untouched
```

A specific hash beats the `*` wildcard. Applying from the spawn hook means every
character the level uses is covered, including variants nobody has identified -
which matters, because targeting one townsfolk hash left the farmers and the
female townsfolk still immortal.

**Write it as plain ASCII.** A UTF-8 BOM is parsed as a phantom rule (harmless,
but it inflates the rule count and is confusing).

### Building the roster

`types` lists every character the current level uses with its live health and
gib flag. `types dump` appends them to `characters_all.txt` in the same format,
so warping through the levels builds a complete, editable roster. That file is
the reference; `characters.txt` is what you actually edit.

## Stock health, as shipped

Swept from levels 0-6. Names recovered by hashing candidate filenames with the
game's own hasher (`strhash`) and matching against live types - most character
prefs are referenced by hash only, so the names came from the animation configs
in `/data/prefs/motionanimconfig/` and the model folders in
`/data/geometry/characters/`.

| character | hash | stock hp |
|---|---|---|
| *(22 protected characters)* | | **100000** |
| townsfolk (Clakkerz) | `5CEE67FD` | 100000 |
| native ("Grubb") | `B6FE5A75` | 100000 |
| native female | `3EE6BB58` | 100000 |
| townsfolk female | `0E997260` | 100000 |
| farmer foster | `606283C9` | 100000 |
| townsfolk storekeeper | `BAF35C16` | 100000 |
| townsfolk sewer worker | `AB444625` | 100000 |
| ugenius | `C766CCA2` | 100000 |
| native rebel (hurt) | `214F50C8` | 100000 |
| vykker doc | `C387D977` | 100000 |
| *(unnamed heavy)* | `F4DC66D8` | 10000 |
| *(unnamed giant boss)* | `CAB666AF` | 5000 |
| gloktigi | `450E9598` | 2000 |
| *(unnamed)* | `300596C6` | 2000 |
| **Jo Momma** | `35179A52` | 1500 |
| **Bad Mortar** | `2B6A3743` | 900 |
| **'Splosives McGree** | `0BB34EB7` | 750 |
| shocktank | `151F4B4E` | 750 *(gibs stock)* |
| **Fatty McBoomBoom** | `57009F0C` | 700 |
| giant slog | `018AD12D` | 550 |
| movie boss | `155A0299` | 500 |
| **Elbows Freely** | `B53065FF` | 400 |
| **Filthy Hands Floyd** | `FF61B694` | 200 |
| castaraider | `C9F81B7E` | 250 |
| wolvark shooter | `FDBD2F9C` | 175 |
| outlaw bomber | `5629AD35` | 150 |
| outlaw nailer | `FF32E523` | 150 |
| outlaw semiauto | `2D8CF05F` | 150 |
| outlaw mortar | `FF0100ED` | 125 |
| wolvark slog handler | `20F622E5` | 125 |
| wolvark grenadier | `F0315D54` | 100 |
| outlaw shooter | `FFFC00CB` | 60 |
| outlaw cutter | `072A64D6` | 45 |
| outlaw sniper | `6A6A4558` | 45 |
| wolvark sniper | `31D1280E` | 45 |
| **sewer slog** | `2F1D4FF5` | **9** |
| **slog** | `7C6717E1` | **3** |

Only three characters gib as shipped: `151F4B4E` (shocktank), `4D82B2A2`, and
`97C44232`. Everything else has `m_onDeathGib = 0`.

The full list, including the 32 still unnamed, is in `characters_all.txt`.

## Commands

| command | effect |
|---|---|
| `npchealth <type> [hp]` | set a type's health (prefs + live NPCs) |
| `npcgib <type> [0\|1]` | gib on death |
| `npchurt <type> [0\|2]` | 2 = cannot be staggered |
| `allnpcs <hp\|-> [gib]` | apply to every character this level uses |
| `types [dump]` | the level's cast with live values, optionally to a file |
| `tuning` | reload `characters.txt` |
| `whereis <type>` | locate every live NPC of a type |
| `resolve <hash>` | hash → prefs object |
| `npcnear` / `nearby [r]` | identify what is next to you |

## Observed behaviour

**Townsfolk are knocked unconscious, not killed.** At low health they drop
moolah and collapse with stars rather than dying - the game's non-lethal capture
path. Gibbing therefore never fires for them even with `m_onDeathGib = 1`,
because they never reach the death state. Whether a damage threshold or a flag
skips the unconscious state is not yet known.

**Heavy characters can crash a level.** Retyping all 157 spawns in level 6 to
the 5000hp boss crashed the game - each carries a boss's model, AI and physics.
`dupetype <hash> <everyN>` retypes only every Nth spawn, which is the usable
form for anything expensive.

## There is no NPC-vs-NPC hostility

Established by observation and by data, because it decides what a "town raid"
mod can be:

* **`m_affGenerally` reads `1` for every character** - townsfolk, outlaws,
  slogs, bosses alike. `m_affList` (`+0x4EC`) is `0` on all of them. There is
  one affiliation, so nothing can be on a different side.
* Retyping a third of Buzzarton into outlaw cutters put ~100 armed hostiles in
  a town of townsfolk. They **ignored each other completely**. No aggression,
  no fleeing, no alarm.
* Killing an outlaw in front of townsfolk makes them attack **the player** -
  they register violence but attribute it to you. There is a witness system,
  and its only suspect is the player.

So "enemy" and "passive" are relationships *to the player*, not to each other.
Every fight in the game is Stranger versus someone. Nothing in the engine
models one NPC deciding another is a threat.

**Consequence:** a raid where outlaws attack townsfolk cannot be produced by
editing prefs or affiliations - there is nothing to point. It requires driving
the AI from outside, which needs the actor-binding problem in
[NPC_SPAWNING.md](NPC_SPAWNING.md) solved first (a verb acts on its context's
actor, and VM instances are per-running-script rather than per-actor).

What *is* achievable without that: hostiles placed in a town at load, plus the
town alarm triggered on command (see below). The enemies still only fight the
player, but the town reacts.

### The AI target IS reachable - but acquisition is filtered

Hostility is not stored on the character; it is stored on the live AI:

```
NPC+0x18   -> VMInstanceInternal   (null while idle; this is the actor binding)
NPC+0xB4   -> MindBasic            (null while idle)
Mind+0x60  -> the target, as a {u16 index, u16 generation} handle
Mind+0x1D0 -> a mirror of it
```

Found by taking the **player's own handle** and scanning an NPC that was
actively shooting at him - an outlaw hunting you is simply an outlaw with your
handle in that field. `findtarget` automates this.

`NPC+0x18` also solves the actor-binding problem that blocked `sendnpc`: the
search had been running the wrong way, scanning 879 VM instances for one that
referenced the NPC. The NPC points at its instance, not the reverse - and only
while it is active, which is why roughly one in ten ever matched.

**Writing another NPC's handle into `Mind+0x60` does change the target.** The
outlaw breaks off from the player, turns, hunts - and eventually barks *"lost
him"*, the game's own line for losing an enemy. So the write is accepted and
the AI acts on it.

**What it will not do is engage.** Retargeted outlaws search but never acquire
a townsfolk, even one a few metres away and in the open. The Mind holds no
cached target position (the floats after `+0x60` are small unit-ish values, not
world coordinates), so acquisition must go through perception - and a townsfolk
is evidently filtered out as not-an-enemy before it can be seen.

The complete target state, all found by scanning an NPC that was chasing the
player for the player's own handle and coordinates:

```
Mind+0x60   target handle {u16 index, u16 generation}
Mind+0x1D0  mirror
Mind+0x340  mirror
Mind+0x3E8  the target's last-known POSITION (3 floats)
```

Writing only the handles makes an NPC hunt blind - it knows WHO but not WHERE,
and gives up with the game's own "lost him" bark. Writing the position too, in
all three handle slots, and re-writing every 20 frames so the AI's own
re-acquisition cannot revert it (`raidmode`), still does not produce an attack.

**The veto is blanket, not species-based.** Tested:

| attacker | victim | result |
|---|---|---|
| outlaw cutter | townsfolk | accepts target, hunts, never engages |
| slog (a predator, 3hp) | townsfolk | identical |

A town containing both slogs and Clakkerz, with 14 active NPCs, had **all 14
targeting the player and none targeting each other** - predator and prey in the
same street, mutually invisible.

So no NPC will engage any other NPC regardless of type, and forcing the target
fields cannot change that. Note this is an **AI** restriction, not a combat one:
NPCs do kill each other with stray explosives, so the damage path between them
works fine. Only deliberate targeting is refused.

### The decoy route also fails

Since the AI will fire at a position and projectiles damage whatever they hit,
the obvious workaround is to leave the target as the player -- so the NPC stays
engaged and willing to shoot -- and write the VICTIM's coordinates into the
last-known-position cache, hoping its shots land on the victim.

Tried with outlaw shooters (`FFFC00CB`, projectile-armed -- cutters are melee
and have nothing to redirect), refreshed every 5 frames. **No effect**: they
kept firing directly at the player, and the NPC count did not fall except for
the player's own kills.

The reason is sound rather than mysterious: `Mind+0x3E8` is a *last-known*
position, consulted when the target is out of sight so the NPC knows where to go
looking. Aiming uses the live target object. A visible target is shot at
directly; an invisible one is searched for, not fired upon. So feeding it a lie
only changes where it walks, and only while it has already lost you.

### What is left

**Retaliation, untested** - whether an NPC damaged by another NPC fights back
the way it does when the player shoots it (confirmed for the player: "he
retargeted me after I attacked him"). NPCs demonstrably do kill each other with
stray explosives, so if retaliation is not player-only, NPC-vs-NPC combat can be
started by causing one accident rather than by forcing targets at all.

Cheap to test in play: stand directly behind a slog while an outlaw shoots at
you, and watch whether the slog turns on the outlaw.

Two further routes both stalled on the same veto:

* `CombatGoto` - sets a goal, executes cleanly, hostility unchanged.
* `TakeDamage({void}|Object)` - "you were hurt by this object", the AI's own
  legitimate path to hostility (shooting an outlaw makes him drop everything and
  come for the shooter). Called with a townsfolk as the source, it executes
  without faulting and the outlaw searches for an attacker - but never acquires
  the townsfolk.

So targeting and threat injection both work at the data level; **perception is a
separate gate**. An outlaw will not see a townsfolk as an enemy no matter what
its Mind says. Finding that filter is the next investigation - likely a
class/species check inside the perception or "can I attack this" code, reachable
by hooking whatever reads the target handle at `Mind+0x60` during an acquisition
attempt.

Also note raiders must be **active** to be commanded at all - an idle NPC has
neither a Mind nor a VM instance. In a 318-NPC town only the handful currently
engaged are reachable (typically 4 of ~105 outlaws).

### The town alarm can be driven

`TownPanicPrefs` (`m_panicForever` at `+0x29`) - setting it turns the town
hostile, rings the alarm bells, and starts the panic spreading. Confirmed
working from the console. Stock values in Buzzarton:

```
steefDistSqr = 49      (Steef triggers panic within 7 units)
beginR = 5   grow = 2   maxR = 161 / 300 / 500
```

Four panic zones per town, each pairing a `TownPanicPrefs` with a live
controller (`+0x0C` points back at its prefs, `+0x10` looks like a member count).

Every audio cue exists **twice** - a Steef version and an "Other" version, all
populated with distinct non-zero hashes:

```
m_alarmCue        E8EEB0B6
m_panicOtherCue   4E51BE40    m_panicSteefCue   A41CF518
m_insideOtherCue  FCF9E705    m_insideSteefCue  4A950424
m_emergeOtherCue  9C872BCA    m_emergeSteefCue  B6DB511A
```

So the game ships a complete second set of panic voice lines for threats that
are not the player-as-Steef. What selects the Other path is not known: it is
not in the prefs, and not in the first `0x30` bytes of the controller.

## Ragdoll

There is none - no ragdoll fields across 1707 reflected offsets, no ragdoll
class among 432 RTTI entries, no ragdoll verb among 348 scripts. Deaths are
animated, with gibbing as the alternative. Adding ragdoll would mean writing a
rigid-body solver and binding it to the HD remaster's skeletons, which is a
project rather than a flag. Gibbing is the cheap route to a death that needs no
animation.

There is also **no blood or decal system** - nothing matching decal, blood,
splat, stain or gore anywhere in the reflected fields or RTTI. Gibs are
`PhysParticleSystem` (physics particles, with `m_fadeOutPercent`), so they do
scatter physically, but they leave no marks.
