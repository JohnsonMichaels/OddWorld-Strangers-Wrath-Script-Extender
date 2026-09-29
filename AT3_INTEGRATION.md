# SWSE 1.1 and Stranger: Armed to the Teeth

For **Racewizard**, and anyone else building a tool on SWSE.

AT3 is the first large project built on SWSE, and SWSE 1.1 was shaped partly by
what AT3 needs: saved facings in the same angle AT3 uses, player speed and
jump, a `misstime` that actually applies, a live alternative to bundle
patching, and a machine-readable way to ask the game questions. This page says
what AT3 can rely on in SWSE 1.1, and the one behaviour change it has to plan
for.

Everything below is checked against the SWSE 1.1 source. Remarks about AT3
itself refer to AT3 v0.3.0's code as we read it - correct us if we misread
anything. The complete command and file reference is
[SWSE_FEATURES.md](SWSE_FEATURES.md); what changed is in
[CHANGELOG.md](CHANGELOG.md); what a tool can rely on in every 1.x release is
[TOOL_CONTRACT.md](TOOL_CONTRACT.md).

---

## The one behaviour change: console-only by default

SWSE 1.1 switches on **only the console** unless `SWSEMods\features.txt` says
otherwise (1.0.x switched everything on). Two consequences for AT3's Global
rules:

| AT3 writes | Switch | Default in 1.1 |
|---|---|---|
| `SWSE Console\aiprefs.txt` - `active = Racketeer` and the `[Racketeer]` section | `aituning` | **auto** |
| `SWSE Console\playerprefs.txt` - `health`, `stamina` | `playertune` | **auto** |

**Nothing for AT3 to change.** These two switches ship as `auto`: each system
runs only while its own file asks for something - an `active = <profile>`
line in `aiprefs.txt`, or any value in `playerprefs.txt` - and stays off
otherwise. AT3 writes exactly those, so its Global Rules take effect at the
next launch without anyone editing `features.txt`. A stock SWSE install asks
for nothing, so it is still console-only.

- A player's explicit `aituning = off` (or `on`) in `features.txt` always wins
  over auto, and AT3 should leave that choice alone.
- With the game running, send `features aituning auto` (or `playertune`)
  through the mailbox to re-check a file AT3 just wrote. Auto is otherwise
  decided at launch. The command also saves `auto` to `features.txt`, so send
  it only when the file says `auto` or has no line for that switch; otherwise
  it would replace the player's explicit choice.
- `features` lists an auto switch as `auto:on` or `auto:off`; `query features`
  reports the live state.

---

## What AT3 can rely on in 1.1

| Thing | Where | Notes |
|---|---|---|
| AI profile file | `SWSEMods\SWSE Console\aiprefs.txt` | `[Racketeer]` works as written; `misstime` now applied |
| Player file | `SWSEMods\SWSE Console\playerprefs.txt` | `health`, `stamina`, plus new `speed`, `jump`, `gravity`, `aircontrol` |
| Placement points | `SWSEMods\SWSE Console\sites.txt` | `label x y z yaw [level]`, written by `writepos`; yaw is AT3's own angle |
| Named positions | `positions.txt` in any mod folder | `label x y z [level] [yaw]`; `writepos` writes `SWSE Console`'s |
| Remote mailbox | `SWSE Console\remote_in.txt` / `remote_out.txt` | same protocol as 1.0.x; on whenever the console is |
| Machine-readable state | `query player\|position\|features\|version\|contract` | one line of key=value |
| Switching systems | `features <name> on\|off [temp]` | also through the mailbox |
| Live prefs edits | `prefs`, `knockback`, `prefs.txt` | the live equivalent of the ammo bundle patches |
| Rescan after writing files | `mods reload` | see below for what it re-reads |
| Install check | `bin\dinput8.dll` + `SWSEMods\features.txt` / `load_order.txt` | AT3's `Test-AT3Install` still passes: both files ship |
| Version check, game closed | `bin\dinput8.dll` version resource | ProductVersion `1.1` (1.0.x has none) |

---

## aiprefs.txt and the Racketeer profile

SWSE reads `aiprefs.txt` from the **last enabled mod folder that has one**
(normally `SWSE Console`). AT3's rewrite - replace the first `active =` line
outside any section, drop and re-append `[Racketeer]` - fits the shipped 1.1
file, which has exactly one `active = off` line before its first section.

| Key | Meaning in SWSE 1.1 |
|---|---|
| `firerate` | multiplier on shots per second: higher = faster |
| `reload` | multiplier on reload time: lower = quicker (AT3's `1 / Reload` is right) |
| `accuracy` | multiplier on the spread: lower = more accurate (AT3's `1 / Accuracy` is right) |
| `misstime` | **milliseconds, 10 = vanilla** - exactly the number AT3 writes |

**`misstime`.** `m_missTime` (`NPCWeaponPrefs +0x1AC`) is stored in seconds and
ships at 0.010 (10 ms) on almost every NPC weapon. SWSE 1.1 reads `misstime` in
the game's standard milliseconds and scales every **firing** enemy weapon from
its own shipped value: new = shipped × misstime / 10. On the usual 10 ms weapon
`misstime = 500` gives exactly 500 ms; the rare weapon that ships longer keeps
its proportion instead of being flattened to 10, and one that ships 0 stays 0 -
so AT3 writing `misstime = 10` alongside other rules changes nothing about miss
time. Leaving the key out, or `-1`, keeps the shipped values, and
`difficulty off` restores them. **SWSE 1.0.x parsed `misstime` and ignored
it**, so on 1.0.x AT3's miss-time setting had no effect through SWSE.

**Who is tuned.** Weapons are reached through their owning character, and any
character at 100000 health (townsfolk, Clakkerz, natives, storekeepers) is
skipped, so only enemies are touched; the player never is. A protected
character that SWSE's character tuning made mortal (`noimmortals`,
`characters.txt`) is still skipped: SWSE remembers every type it lowered from
100000.

**When.** With `aituning` on, the `active` profile is applied once per level,
4 seconds after the level's player appears, on a worker thread. To apply a
change while a level is running:

| After AT3 writes... | Send |
|---|---|
| new Racketeer values, Racketeer already in force | `aitune` (re-reads the file and re-applies the profile in force) |
| `active = Racketeer` where it was off | `aitune` then `difficulty Racketeer` for this level. With the switch on `auto` (as shipped), `features aituning auto` instead switches the per-level apply on and applies the profile once the level has been up 4 s |
| `active = off` | `difficulty off` (restores the shipped values exactly) |

`difficulty` and `aitune` work even with the `aituning` switch off; only the
automatic per-level apply needs it.

**Limits worth knowing.**
- **Size:** SWSE reads at most **32 profiles**, from the first **64 KB** of
  the file. 1.0.x stopped at 8 profiles and 8 KB, so AT3's appended
  `[Racketeer]` was dropped for a user with five profiles of their own. The
  shipped file has 3 profiles, so `[Racketeer]` is the 4th.
- **Decimal commas:** AT3 formats its numbers with the Windows culture
  (`"{0:0.0##}" -f $x`), so a French or German system writes
  `reload = 0,667`. SWSE 1.1 reads a comma as a decimal point, in
  `aiprefs.txt` and `playerprefs.txt`. SWSE 1.0.x read `0,667` as 0: instant
  reloads and perfect aim. Formatting with the invariant culture in AT3 would
  still be the robust fix for anything else that reads the file.
- **Re-applying is safe:** each tuned value is computed from the shipped
  original, which SWSE keeps for exactly as long as that prefs record exists.
  `aitune` and a `difficulty Racketeer` sent twice never compound, and
  `difficulty off` lands on the shipped numbers - also after a level change.

---

## playerprefs.txt

| Key | What SWSE 1.1 sets |
|---|---|
| `health` | max **and** current health (all three stored copies) - applying fills the bar |
| `stamina` | max and current stamina |
| `speed` | run speed, the player's own motion objects |
| `jump` | jump height, the player's own motion objects |
| `gravity` | fall gravity - `GlobalMotionPrefs`, so everyone, NPCs included |
| `aircontrol` | midair steering - `GlobalMotionPrefs`, everyone |

`key = value` or `key value`; blank, `-`, `default` or absent keeps the game's
own value - so AT3's `health  =` (no value) is read correctly as "leave it".
The last enabled mod that provides the file wins (normally `SWSE Console`).

**When.** With `playertune` on, once per level, 3 seconds after the player
appears (after the game's difficulty-based setup: 600 / 300 / 150 health for
easy / normal / hard). If the game later resets max health or stamina to its
own number - a checkpoint reload, say - SWSE applies the value again. The
game's values are captured first, so `playertune restore` or switching the
feature off puts them back. `playertune reload` re-reads the file and applies
it immediately.

**One thing in AT3:** `Set-AT3PlayerPrefs` rewrites the whole file with only
`health` and `stamina`, which erases any `speed`, `jump`, `gravity` or
`aircontrol` line the user added (and the notes, if they started from the
template in `SWSE Console\examples\`). Rewriting only
its own two lines - the way AT3 already treats `aiprefs.txt` - would keep them.

---

## Positions, facing and sites.txt

**Yaw is AT3's angle.** SWSE 1.1 reports the player's facing as the rotation
angle of its world matrix about Z, `atan2(m3, m0)`, in degrees 0..360 - the
angle the game's level records store for placed objects and the one
`spawn_prop.py` reads (`yaw_of`) and builds matrices from
(`matrix_from_yaw`). A yaw from `pos`, `query` or `sites.txt` can go straight
into a record: the matrix it builds is exactly the one `Actor::SetFacing`
stores for the player. In compass terms 0 = facing -Y, 90 = +X, 180 = +Y,
270 = -X. Details: `swse/research/PLAYER_FACING.md`.

| Command | What it does |
|---|---|
| `pos` | X Y Z and yaw |
| `yaw [deg]` | read or set the facing |
| `writepos <label>` / `savepos <label>` | save the spot and facing by name, to file |
| `goto <label>` / `tp <label>` | go there, facing restored |
| `tpxyz <x> <y> <z> [yaw]` | teleport to coordinates |
| `positions` | list every label with yaw and level |

**The files.** Both are read from every enabled mod folder; a later
definition of a label wins.

```
positions.txt   label   x y z   [level]   [yaw]      the two optional columns in either order
sites.txt       label   x y z   yaw   [level]         the order AT3 reads
```

`writepos` appends the point to `SWSE Console\positions.txt` and - **only when
the facing could be read**, so a made-up 0 is never recorded as a facing - to
`SWSE Console\sites.txt`: exactly the two files AT3's `read_marker` reads.

**The level column** is `-` unless SWSE knows the level, and it only learns
the level name from its own `warp` command - after loading a save it does not
know it, and after a warp followed by a normal level change the name can be
stale. Treat it as a hint.

**A small thing in `spawn_prop.py`:** `--yaw` defaults to `0.0`, so in
`yaw = a.yaw if a.yaw is not None else (logged_yaw or 0.0)` the yaw read from
`sites.txt` is never used. A default of `None` would pick it up - and with 1.1
it can be used as it is.

---

## The remote mailbox

SWSE polls `SWSEMods\SWSE Console\remote_in.txt` about eight times a second
while the console feature is on (it is by default).

```
remote_in.txt                    remote_out.txt
7                                7
query player                     > query player
features aituning on temp        levelup=1 epoch=2 level=- x=... yaw=137.90 ...
                                 > features aituning on temp
                                 aituning ON: aiprefs.txt: 4 profile(s), active = Racketeer
                                 (this session only - features.txt unchanged)
                                 <<END>>
```

* The first line is a sequence number. A request runs when its number **or the
  file's write time** changes, and at launch SWSE takes the file already on
  disk as seen without running it, so a leftover request never replays.
  Because the write time counts too, AT3's `Send-SWSECommand` - which restarts
  at 1 every session - is not ignored when an old file happens to carry the
  same number; starting from the number on disk plus one (as `tools/swse.ps1`
  does) is still the clean way.
* The following lines, up to 256, run as one sequence, exactly as if typed;
  lines starting `#` are skipped; a line may be up to 239 characters. The
  reply says if any lines were cut or dropped.
* **End the file with a newline.** Both `Send-SWSECommand` and
  `tools/swse.ps1` already do. SWSE runs a request only once it is completely
  written: when the file ends with a newline, or has sat unchanged for about
  120 ms. A request read mid-write could otherwise run twice.
* The reply starts with the sequence number, echoes each command as
  `> command`, then its output, and ends with `<<END>>`, all CRLF. A command
  that faults writes `*** FAULTED - command crashed (game survived) ***` and
  the game carries on.
* A `wait <ms>` line delays the lines after it; those, and anything scheduled
  with `after`, run later and print to the console, not into the reply.

### query - key=value for tools

| Request | Reply (one line; a field is left out when it cannot be read) |
|---|---|
| `query` or `query player` | `levelup=1 epoch=2 level=- x=... y=... z=... yaw=... health=... healthmax=... stamina=... staminamax=... moolah=...` |
| `query position` | `levelup=... epoch=... level=... x=... y=... z=... yaw=...` |
| `query features` | `console=on graphics=off hdtextures=off hitreact=off foliage=off aituning=off triggers=off npctuning=off playertune=off prefsedit=off raytrace=off` (the live state: an `auto` switch reads `on` or `off`) |
| `query version` | `swse=1.1 build=Steam_HD` (`build=unknown` on a game build SWSE does not recognise) |
| `query contract` | `contract=1 swse=1.1` - see [TOOL_CONTRACT.md](TOOL_CONTRACT.md) |

`levelup=0` means no level is loaded (menu, loading screen). `epoch` counts
levels since launch, so a change in it means a new level. Coordinates carry
three decimals, yaw two, health and stamina one. On 1.0.x `query` does not
exist - the reply is an "unknown" line - which is a simple way to tell the
versions apart while the game runs. With the game closed, the DLL's version
resource answers: `(Get-Item "bin\dinput8.dll").VersionInfo.ProductVersion`
is `1.1` for a 1.1 build (file version 1.1.0.0), and empty for 1.0.x, whose
DLLs carry no version resource. `status` is the same information as `query`,
laid out for people.

`query`, `status` and `pos` are cheap. Commands that scan memory - `weapons`,
`npcguns`, `findai`, and `prefs` with an `@Class` target - run on the game's
render thread and can hold a frame for a second or more: fine when a person
types them, but not something to poll.

### Other commands that suit a tool

| Command | Use |
|---|---|
| `features <name> on\|off [temp]` | switch a system; without `temp` it is saved |
| `mods` / `mods reload` | which folders provide which SWSE files / rescan after writing files |
| `hide` / `show` | close / open the console overlay |
| `log <text>` | write `LOG: <text>` into `bin\swse_log.txt` - handy to mark what AT3 did |
| `exec <file>` | run a text file of commands (relative to `SWSE Console`) |
| `aitune`, `difficulty <name>\|off` | AI profile reload / apply / restore |
| `playertune reload\|apply\|restore` | player values |
| `prefs ...`, `knockback ...` | live prefs edits, below |

`mods reload` rescans the folders and `load_order.txt`, reloads positions,
sites and script commands, and re-reads the files of the systems that are on
(`aiprefs.txt`, `triggers.txt`, `playerprefs.txt`, `prefs.txt`,
`console.txt` / `characters.txt`). Re-reading is not re-applying: use the
commands in the tables above to apply immediately.

---

## Live equivalents of AT3's bundle patches

AT3 writes ammo knockback straight into the serialized records in
`data\bundles` (with `.knockbak` backups). SWSE 1.1 can make the same change to
the **loaded** objects instead:

```
knockback                              all 31 ammo types, current values
knockback boombat 40 25                what boombats hit at 40, you at 25 - this level
knockback all - 0 keep                 nothing you fire knocks you back, every level
prefs set weapons/damagedynamite m_maxKnockSpeedPlayer 25
prefs restore                          everything back to shipped
```

or as lines in any mod's `prefs.txt`, re-applied every level with the
`prefsedit` feature on:

```
/data/prefs/weapons/damagedynamite.txt   m_maxKnockSpeedPlayer   25
```

| | AT3 (serialized record in the bundle) | SWSE (loaded object) |
|---|---|---|
| `m_maxKnockSpeed` | record+164 | `+0x140` |
| `m_maxKnockSpeedPlayer` | record+168 | `+0x144` |
| finding the record | ammo hash at +0, marker `0x000B4265` at +0x14 | the game's resource registry (a sorted list of every loaded resource), by path hash |
| vanilla values | `AmmoKnockback.csv` | the same - all 31 read live match |

**What `-1` means.** Both fields belong to `BoltPrefs`, which every ammo type
and every NPC weapon inherits. The engine's knock getter (`0x4A1490`) uses
`m_maxKnockSpeedPlayer` when it is 0 or more, and `m_maxKnockSpeed` otherwise,
and only knocks when the result is above 0. So the shipped `-1` on every ammo
type means "throw the player exactly as hard as an NPC", and `0` means "never
knock the player" - not "unset, so explosions never launch you". A real
number sets the player's own knockback.

Both sides hash paths the same way: AT3's `gamehash.py` and SWSE's in-process
hasher (`strhash <path>` in the console) agree.

The trade-off, for AT3 to weigh: SWSE's edit touches no game file, needs no
backup, and is undone by `prefs restore` or switching the feature off; but it
exists only while SWSE runs with `prefsedit` on, and it lands 4.5 seconds into
each level, so the first seconds of a level use the shipped values. A bundle
patch is in force from the first frame, SWSE or not. The prefs editor reaches
any loaded record the same way (`prefs find|get|dump`), which may also be a
way to show live values in AT3's UI. The class and field table behind it is
`swse/research/REFLECT_FIELDS.tsv` - 5,697 fields in 191 classes with exact
classes, types and inheritance, and dotted names for embedded objects
(`m_spAIPrefs.m_sightCombat.m_seeDistance`), replacing the noisier
`FIELD_OFFSETS.tsv`. How objects are found: `swse/research/PREFS_EDITOR.md`.

---

## What SWSE learned from AT3

Lining AT3's findings up against SWSE's live view produced a list of verified
facts - the facing frame, prefs objects' own hash at `+0x0C`, the ammo classes
and their serialized-versus-runtime offsets, `m_missTime` in seconds, the path
hash, the `.lvl` record chain - and exposed several latent SWSE bugs along the
way. They are credited to you in
[SWSE_FEATURES.md](SWSE_FEATURES.md#what-we-learned-from-stranger-armed-to-the-teeth-at3)
and [CHANGELOG.md](CHANGELOG.md). Thank you.

The rest of what AT3 found is mapped onto live offsets, with proposed SWSE
commands, in `swse/research/AT3_DISCOVERIES.md`. That document is static
analysis of the exe and of retail records - nothing in it has been run in the
game yet - and it ends with a few points that may be useful to you:

* `ser+89` / `ser+93` are `m_damage` / `m_damageDestructable` in the engine's
  own parameter order - the two look swapped in AT3's weapon fields, though
  AT3's own club test pointed the other way, so this one needs an in-game
  check.
* ANC+127 is `m_armadilloDamageMultiplier` and ANC+132
  `m_spiderImmobilizeMultiplier`.
* The spray dials AT3 leaves unnamed are, in order, `m_spawnRadius`,
  `m_dynamic`, `m_tumble`, `m_inheritSpawnersVelocity`, `m_vSpeed` (+
  randomness) and `m_hSpeed` (+ randomness).
* Homing is `m_magnetismOverride`.

Questions and anything that does not match what you see: the SWSE Discord,
https://discord.gg/TWHzP924wE .
