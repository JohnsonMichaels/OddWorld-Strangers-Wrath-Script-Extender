# NPC AI - what difficulty does, and what is actually tunable

Research question: **does the difficulty setting do anything beyond health, and
can NPC thinking rate, rate of fire, or "smartness" be changed?**

Short answer: difficulty is *not* an AI system - it touches no AI parameter at
all. But the engine carries a large per-character AI tuning surface that exists
independently of it, including a four-state alert model with its own perception
values per state. That is the lever worth pulling.

## What the difficulty menu actually sends - SOLVED

`global_guidifficulty.smb` carries an **uncompressed** SWF (`FWS`, v6 =
ActionScript 2) at offset `0x1E3004`, 45,124 bytes. Extracted to
`hd_work/difficulty_menu.swf` and disassembled; no external tool needed.

The menu defines its options as plain integers:

```
OPTION_EASY   = 0
OPTION_MEDIUM = 1        <- cursor starts here, so MEDIUM is the default
OPTION_HARD   = 2
OPTION_BACK   = 3
OPTION_COUNT  = 4
```

`chooseOption` compares the cursor against each and calls `ExitFade`, which
stores `on_exit_arg1` / `on_exit_arg2` and, once the fade completes, issues an
`fscommand`. In AS2 that compiles to `ActionGetURL("FSCommand:<cmd>", "<arg>")`,
and decoding every one of those in the menu gives the whole contract:

```
EASY    ->  fscommand("new_game", "0")
MEDIUM  ->  fscommand("new_game", "1")
HARD    ->  fscommand("new_game", "2")
BACK    ->  fscommand("goto_level_select", "")
```

**Difficulty is a single integer argument to `new_game`.** Nothing else is
passed - no health value, no multiplier, no per-system settings. The menu's
entire contribution is one number in the range 0..2.

Two consequences:

* A **4th difficulty** is `new_game` with `"3"`. The SWF would need its
  `OPTION_*` constants and `OPTION_COUNT` raised (and a fourth text row, which
  already exists as the `text_bright<N>` / `option_highlight<N>` pattern), or
  the value can be injected without touching the SWF at all by intercepting the
  fscommand.
* Because the menu only ever sends a number, **whatever difficulty does is done
  entirely on the game side from that integer** - which is what makes "does it
  only change HP" answerable by finding the readers of that one value.

## Difficulty is not part of the prefs system

Searched exhaustively, and the negative result is solid:

* **Zero** of the **1910** reflected fields contain "difficult", "skill",
  "easy" or "hard" in their names.
* The only `Difficulty` symbol in the executable is a UI callback,
  `?AVMyCallback@Difficulty@@`, alongside `/data/ui/difficulty_menu.swf` and
  the `global_guidifficulty.smb` bundle. Menu code, not gameplay code.
* No `600 / 300 / 150` triple exists in the executable, as floats or ints,
  adjacent or otherwise. There is no difficulty→health table compiled in.
* `config.txt` holds no difficulty key - the setting lives in the save.

So difficulty cannot be reaching NPC fire rate, accuracy or perception through
prefs, because there is no prefs field for it to write. Whatever it changes, it
changes at the point of damage or at player setup, not in the AI.

`GPrefs` has no difficulty field either, but it does have **`m_logAI`** - the
engine ships an AI logging channel. That is the most direct instrument for the
"rate of thinking" question and has not been switched on yet.

## The reflection schema had to be redone first

`REFLECTION_SCHEMA.md` states its own caveat: *"Grouping is heuristic (nearest
class-like token before each field run) so treat boundaries as approximate."*
That is fine for browsing and actively harmful for addressing memory. It put
the perception fields under a class called `CoverDuration` - a name that has
nothing to do with sight - and reading a live NPC with its numbers returns
another character's hash, which looks like data rather than like a mistake.

The executable registers every reflected field with its offset as a **literal
in the instruction stream**, so the exact answer was always available:

```
6a 0c                 push 0Ch                 ; sizeof(descriptor)
e8 <rel32>            call operator new
c7 06 <typedesc>      mov  [esi],   <type>
b8 <nameVA>           mov  eax,     <field name string>
c7 46 08 <offset>     mov  [esi+8], <FIELD OFFSET>     <-- exact
```

`tools/reflect_dump.py` reads this: **1910 fields across 49 registration runs**,
each with a offset that is read, not inferred.

```
python tools/reflect_dump.py                     summary
python tools/reflect_dump.py --field m_fireRate  locate one field
python tools/reflect_dump.py --csv out.csv       everything
```

### Splitting classes correctly

The first attempt split runs on a gap in code position, and that was wrong:
several classes are registered inside one function, so runs came out with
three different fields all claiming offset `0x4`. The real signal is that **a
class registers its fields in increasing offset order**, so a decrease is a
class boundary. That one rule took the dump from 49 merged blobs to **294
clean runs**, and it is self-checking - a correct run comes out strictly
ascending.

The two classes this research needed fall straight out, both perfectly
contiguous, which is the evidence that the split is right.

## The perception model - four alert states  (run 126 + run 125)

This is the real "make them smarter" surface, and it is two classes.

**`SightParams`** - an 8-field, 32-byte block (run 125), strictly ascending:

| offset | field | meaning |
|---|---|---|
| `+0x00` | `m_6thSenseDistance` | detection with no line of sight |
| `+0x04` | `m_seeDistance` | ordinary sight range |
| `+0x08` / `+0x0C` | `m_seeAbove` / `m_seeBelow` | vertical reach |
| `+0x10` / `+0x14` | `m_horizontalAngleDeg` / `m_verticalAngleDeg` | the view cone |
| `+0x18` | `m_instantSightDistance` | seen immediately, no reaction delay |
| `+0x1C` | `m_hideVolSeeDistance` | seeing into hiding volumes |

**The AI perception prefs** (run 126) embeds **one of those per alert state**:

| offset | field |
|---|---|
| `+0x004` | `m_sightNormal` |
| `+0x0A8` | `m_sightAgit` |
| `+0x14C` | `m_sightCombat` |
| `+0x1F0` | `m_sightPanic` |
| `+0x294` | `m_allowPanic` |
| `+0x298` / `+0x29C` | `m_minWaitForConversation` / `m_maxWaitForConversation` |
| `+0x2A0` | `m_relaxAgitatedToNormal` |
| `+0x2A8` | `m_attackParams` |

**The four blocks are exactly `0xA4` apart** - `0x4 → 0xA8 → 0x14C → 0x1F0`,
three identical strides. That regularity is arithmetic, not a guess: it is a
4-element array of 164-byte per-state blocks, of which the first 32 bytes are
`SightParams` and the remaining 132 are per-state data not yet broken out.

So an NPC sees *further and wider once it is already agitated*, has a literal
sixth-sense radius, and decays back to calm on a timer. None of it is touched
by difficulty.

## Rate of thinking, and rate of fire

* **`m_checkEverySeconds`** (run 178, `+0x68`) - a periodic AI check interval,
  sitting with `m_major`, `m_minor`, `m_relax`, `m_radius`,
  `m_restrictToSpecificGuy` and `m_restrictToBrainState`. That last name
  confirms an explicit **brain-state machine**, and this is the decision-rate
  dial.
* **`m_aiLowDetail` / `m_aiHighDetail`** - the engine's own AI level-of-detail:
  it already varies how much thinking distant NPCs do.

**`NPCWeaponPrefs`** (run 114) is contiguous in 4-byte steps - the clearest
confirmation the extraction is correct:

| offset | field |
|---|---|
| `+0x178` | `m_weaponType` |
| `+0x17C` | **`m_fireRate`** |
| `+0x180` | `m_fireFromTextkeys` |
| `+0x184` / `+0x188` | `m_reloadTime` / `m_reloadTimeMax` |
| `+0x18C`…`+0x194` | `m_hitConeAngleXY` / `AngleZ` / `Height` |
| `+0x198` | `m_minDistance` |
| `+0x19C` / `+0x1A0` | `m_spawnPref` / `m_spawnCount` |
| `+0x1A4` | `m_countdown` |
| `+0x1A8` | **`m_accuracyWidth`** |
| `+0x1AC` | **`m_missTime`** |
| `+0x1B0` | `m_actorTargetDetail` |
| `+0x1B4` | `m_leadWithAverageVelocity` - target leading |

`m_missTime` and `m_accuracyWidth` are the pair worth noting: the engine models
**deliberate missing**. That is exactly the dial a difficulty system would want,
and it does not use it.

## Weapon prefs - reached by VTABLE, not by shape

A shape-based scan for `NPCWeaponPrefs` produced nothing but false positives:
fire rates of 0, cones of `1x1`, twenty identical records. The weapon block has
no cross-state invariants to lean on the way the perception object does, and
"a run of plausible floats" is not identification.

RTTI settles it. `.?AVNPCWeaponPrefs@@` → **vtable `0x776454`** (RVA
`0x376454`), 37 methods. Objects are then matched on their vtable pointer,
which is real identification and safe to write through.

**The method self-checks:** the same walk returns `NPCPrefs` = `0x767E1C`,
matching the `0x367E1C` already hardcoded in `PrefsOfNpc` from unrelated
earlier work.

### Joining a character to its gun

Both classes carry their own path hash at `+0x0C`, so:

```
NPCPrefs +0x498  m_rangedWeapon   ==   NPCWeaponPrefs +0x0C  (own hash)
```

`npcguns` performs that join and prints health and kill bounty alongside,
because the recovered character NAMES in `NPC_TUNING.md` are indexed by stock
health. Measured in region 03:

| hash | hp | bounty | fire rate | name |
|---|---|---|---|---|
| `2D8CF05F` | 150 | 10 | 10000 ms | outlaw semiauto |
| `B5A32E92` | 200 | 10 | 3000 ms | *unnamed* |
| `FFFC00CB` | 60 | 5 | 1300 ms | outlaw shooter |
| `35179A52` | 1500 | 400 | 1000 ms | Jo Momma |
| `072A64D6` | 45 | 3 | 1000 ms | outlaw cutter |
| `B28A48AA` | 500 | 300 | 500 ms | *unnamed* |
| `4D82B2A2` | 600 | 0 | 500 ms | *unnamed (gibs stock)* |
| `6A6A4558` | 45 | 5 | 399 ms | outlaw sniper |
| `FF0100ED` | 125 | 8 | 0 | outlaw mortar |
| `5629AD35` | 150 | 8 | 0 | outlaw bomber |
| `2B6A3743` | 900 | 300 | 0 | Bad Mortar |

**`m_fireRate` is not a plain shot delay for every weapon.** "Outlaw semiauto"
reads 10000 ms and the sniper reads 399 ms, which is backwards from how they
play. `m_fireFromTextkeys` sits at `+0x180`, so animation-driven weapons very
likely take their timing from animation text keys and ignore this field. Treat
a non-zero fire rate as tunable and verify in play; do not assume every entry
is live.

### m_missTime is a dead end - measured, not assumed

Earlier notes here called it "probably the single biggest lever." **That was
wrong.** Read live, it is **10 ms on almost every character**, with a single
outlier at 500 ms. The engine has the capability to make NPCs deliberately
miss and essentially does not use it. It was speculation from a field name; the
measurement retires it.

## The modder-facing surface

Everything a difficulty modifier would want to reach exists and is addressable:

| want | field | class |
|---|---|---|
| how far NPCs see | `m_seeDistance`, `m_instantSightDistance`, `m_6thSenseDistance` | SightParams, ×4 states |
| how wide they see | `m_horizontalAngleDeg`, `m_verticalAngleDeg` | SightParams, ×4 states |
| how often they decide | `m_checkEverySeconds` | run 178 |
| how fast they shoot | `m_fireRate`, `m_reloadTime` | NPCWeaponPrefs |
| how well they shoot | `m_accuracyWidth`, `m_missTime`, `m_leadWithAverageVelocity` | NPCWeaponPrefs |
| how long they stay alert | `m_relaxAgitatedToNormal` | run 126 |
| health | `m_health` `+0x448` | NPCPrefs (already shipped as `npchealth`) |

Per-character, because these hang off each character's prefs - so a modifier
can be broad ("all outlaws see 30% further") or narrow, and it composes with
the existing `characters.txt` tuning file.

## What is verified live, and what is not

**Verified against the running game:**

* `NPCPrefs +0x448` is `m_health` (already established in `NPC_TUNING.md`),
  which anchors that run as genuinely NPCPrefs.
* `NPCPrefs +0x498` is `m_rangedWeapon` and it is **real**: it varies per
  character across the live cast (`57664213`, `8361543A`, …) and reads as the
  unset sentinel on melee-only characters.
* **`2DFD1072` is the engine's "unset" sentinel** for hash-valued fields. It
  does not resolve. Any tool reading these fields must special-case it.
* `m_spAIPrefs` reads the **same value on every character** examined, which is
  consistent with one shared AI archetype - but see below.

**The blocker, and it is now diagnosed: `ResolvePrefs` is NPCPrefs-specific.**

Feeding it `m_rangedWeapon`'s hash returns an object, and that object's vtable
is `moduleBase + 0x367E1C` - the **NPCPrefs** vtable - the same as the
character's own prefs and the same as whatever `m_spAIPrefs` resolves to. All
three come back as NPCPrefs.

So the resolver is not returning a weapon or an AI object at all. It looks up
the hash in the NPCPrefs table and hands back an unrelated *character* record
instead of failing, which is precisely why the weapon offsets read as
direction vectors (`0.707` = cos 45°): correct offsets, wrong object.

That also retires the earlier theory that the field offsets were misattributed.
They are fine. `m_rangedWeapon` at `+0x498` is demonstrably real - it varies
per character and reads as the unset sentinel on melee-only ones.

**Solved - by shape instead of by hash.** The object does not need the
resolver. It has a near-unique signature: four sight blocks at a fixed `0xA4`
stride, each opening with a sixth-sense distance and a sight distance and
carrying two view-cone angles in degrees. Four of those in a row, all
self-consistent, does not occur by accident. `findai` scans the heap for it.

```
> findai 1500
  14445F28  normal: 6th=10 see=50  cone=90x90
  14446F18  normal: 6th=2  see=115 cone=90x90
  14447468  normal: 6th=7  see=65  cone=70x110
```

Real objects, plausible values, and the offsets read correctly - which
independently confirms both the extracted layout and the `0xA4` stride.

*(Known rough edge: overlapping matches are reported at `+4` slides of the same
object. Dedupe by rejecting a hit within `0x20` of the previous one.)*

## DEFAULT VALUES - read live

From `14445F28`, a typical outlaw. All four states dumped with `peek`:

| field | normal | agitated | combat | panic |
|---|---|---|---|---|
| `m_6thSenseDistance` | 10 | 10 | 10 | 10 |
| **`m_seeDistance`** | **50** | **100** | **100** | **75** |
| `m_seeAbove` / `m_seeBelow` | 1001 | 1001 | 1001 | 1001 |
| `m_horizontalAngleDeg` | 90 | 90 | 90 | 90 |
| `m_verticalAngleDeg` | 90 | 90 | 90 | 90 |
| `m_instantSightDistance` | 10 | 10 | 10 | 10 |
| `m_hideVolSeeDistance` | 1 | 2 | 3 | 3 |

**The alert model is now proven, not inferred.** An NPC sees **50** units at
rest and **100** once agitated - it literally doubles - settling to 75 in
panic. Its ability to see into hiding volumes triples from 1 to 3 as it goes up
the alert ladder. The view cone stays 90°×90° throughout, and vertical reach is
effectively unlimited (1001).

Object tail:

| offset | field | default |
|---|---|---|
| `+0x294` | `m_allowPanic` | 1 (true) |
| `+0x298` / `+0x29C` | `m_minWaitForConversation` / `m_max…` | 45 / 60 |
| `+0x2A0` | `m_relaxAgitatedToNormal` | 200 |
| `+0x2A8` | `m_attackParams` | 2 |

Other objects found in the same level show the per-character spread the tuning
surface is for: one sees **115** units at rest, another **65** through a
70°×110° cone - a tall, narrow field of view.

Until then, **`ai <hash>` prints the hashes and what they resolve to, and
deliberately does not decode field values.** Printing "fireRate=707ms" derived
from a direction vector would look exactly like a measurement.

## Next steps, in order

1. **Turn on `m_logAI`.** It is a shipped debug channel and would answer the
   thinking-rate question by observation instead of inference.
2. **Split runs on function boundaries** rather than a byte gap in
   `reflect_dump.py` - each registration run is one function, so scanning back
   to the prologue gives true class boundaries and would settle whether
   `+0x118` belongs to NPCPrefs at all.
3. **Identify the perception class by vtable** and find live instances with the
   existing `vtscan`, then work back to how a character reaches its own.
4. Only then wire setters. `ai <hash> <field> <value>` already exists and is
   wired to `SWSE_AiSet`, but it must not be trusted until step 2 or 3 lands.

## Commands

| command | what it does |
|---|---|
| `ai <hash>` | dump a character's AI-related prefs and linked hashes |
| `resolve <hash>` | resolve any prefs hash to its object |
| `peek <addr> [n]` | hex/float/ascii view - how the offsets above were tested |
| `npcnear` | identify a nearby character to get its hash |
| `spawntypes` | every character type present in this level |
