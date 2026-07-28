# NPC spawning, retyping and control

How NPCs come into being in Stranger's Wrath, what can be changed about them,
and - importantly - what cannot. Several conclusions here are negative results;
they are recorded because each one closes off a direction that looks obviously
promising and costs hours to disprove.

## The headline: the engine never creates NPCs at runtime

Every NPC in a level exists from the moment that level finishes loading.
Ambushes, boss reinforcements and "enemies coming through a door" are all
**pre-placed NPCs being activated or walked out of hiding** - nothing is
constructed mid-game.

Measured three independent ways, with `npcspy` armed (hit counter on
`SpawnNPCFromTag`) and the live NPC population sampled:

| situation | spawn-routine calls | NPC count |
|---|---|---|
| sleg ambush during exploration | 0 | falling |
| Jo Momma boss fight, 2 min sampled | 0 | 293 → 284, monotonic |
| enemies emerging from arena tunnels | 0 | 278 → 274, monotonic |

The count only ever falls, as the player kills things. This is why
"spawn an NPC on demand" cannot be made to work: the machinery does not exist.

## The spawn path (load time)

`SpawnNPCFromTag` at **RVA 0x184D90**, `this = NPCTag`, one stack argument.

```
call 0x3590                    ; a global; [eax+0x30] must be nonzero
cmp  dword ptr [esi+0x0C], 1   ; m_countToSpawn must be exactly 1
cmp  byte  ptr [esi+0x22], 0   ; must be nonzero
lea  ecx, [esi+8]              ; the type handle
call 0x23880                   ; resolve handle -> NPCPrefs
call 0x25A50                   ; the factory: builds the NPC
call 0x63010                   ; register it into the world
```

The guard at `0x3590` reads 1 both during a load and in the world, so it is not
what gates spawning - checked to rule it out.

Its caller is a thunk at **0x182EF0**:

```
mov eax, ecx            ; this = InstancedObjectTag
mov ecx, [eax+0x78]     ; the NPCTag lives INSIDE it at +0x78
push eax                ; arg0 = the InstancedObjectTag
call [vtable+0x18]      ; -> SpawnNPCFromTag
```

So the pair is: an **InstancedObjectTag** (0x7C bytes, ctor at `0x182DA0`) that
owns an **NPCTag** (0x5C bytes, ctor at `0x184C20`) at `+0x78`. RTTI names both;
`arg0` is *not* a GeometryInst, which cost several rounds of faults to discover.

### Constructed objects do not work - do not retry this

Building both objects with the game's own constructors, linking them, filling
the fields the disassembly reads, and calling the thunk **succeeds and produces
nothing you can see**:

* the routine returns 1 (its success flag, set only after registration)
* new NPC objects appear in memory with the correct type and position
* they are not in the world - invisible, non-interactive

Tested both after a load and *during* one (`buildtest`, which runs the identical
construction from inside the load hook). Constructed objects fail in both, while
the game's own objects work in both. Timing is not the variable; the objects are.

The likely reason: registration parents the NPC to the InstancedObjectTag, and a
constructed one is an orphan - not in the level's object tree, so whatever it is
attached to is never drawn or ticked. Fixing which fields get copied does not
help and was tried extensively.

## What does work: retyping the game's own tags

The hook runs before the trampoline, so writing `NPCTag+0x08` in flight changes
what the level spawns - using entirely the game's own objects.

* **`dupetype <hash>`** - every spawn tag in the level becomes that character
* **`npcdupe <n>`** - re-invokes the routine n extra times per tag

Verified: 318 → 953 NPCs with `npcdupe 2` (visually confirmed), and whole levels
retyped to cutters, Vykker Docs and giant slogs.

Two caveats found the hard way:

* **Retyping is total.** Friendly and hostile spawns share the mechanism, so a
  retyped level replaces townsfolk too. Attachments (hats) come from the tag,
  not the character, so cutters inherited the farmers' hats.
* **Level scoping.** A character can only populate a level that already loads
  it somewhere. Forcing a foreign type **crashes the level** - the hash resolves
  but its assets are not in the bundle. Same rule as the crossbow upgrade
  needing `region_04`. `dupetype` now refuses hashes not harvested from the
  current level unless given `force`.

## Identifying characters

Types are path hashes, not names. The hasher is at **0x24D920** (table-driven
CRC-32 variant, `'/'`→`'\'`, lowercased, plus a final round mixing in the
length). Reimplementing it offline failed against five known-good pairs, so it
is called in-process instead - `strhash <path>` hashes a candidate and reports
whether it matches a type the current level spawns.

Most character prefs are referenced by hash only; just 20 appear as strings
anywhere in the game files. Guessing filenames works well - `vykkerdocprefs.txt`
appears in no file yet hashes exactly onto a live type.

| character | hash |
|---|---|
| outlawcutter | `072A64D6` |
| outlawshooter | `FFFC00CB` |
| wolvarkshooter | `FDBD2F9C` |
| wolvarkgrenadier | `F0315D54` |
| wolvarksniper | `31D1280E` |
| vykkerdoc | `C387D977` |
| sleg | `7C6717E1` |
| sewersleg | `2F1D4FF5` |
| giantsleg | `018AD12D` |
| elbowzfreely | `59DB2D18` |
| outlawboss_elbowsfreely | `B53065FF` |
| sloghandlerspawned | `E2D7415C` |
| armadillo | `76C75C78` |
| fuzzle | `16956EF7` |
| skunk | `C97B758C` |
| spider | `F73A6BF0` |
| squirrel | `1833B954` |
| stingbee | `1BC27DCA` |
| sulphurbat | `55F1EA59` |

`NPCPrefs` stores its own hash at **+0x0C**, so any live NPC can be identified
directly (`npcnear`) without a name.

## Object arguments to script verbs

79 of the game's 348 script verbs take an `Object`, and the generated call table
skipped every one - it only handles scalars. Objects are **handles, not
pointers**, which is why passing an address never worked.

`0x168540` decodes them:

```
index      = u16 at Value+0x0C
generation = u16 at Value+0x0E
entry      = 0x9D55F0 + index*6      ; pointer at +0x00, generation at +0x04
```

Generation must match or it yields null. Writing that pair into an injected
`VmValue` at `+0x0C` works - handles resolve and calls execute cleanly.

This unlocks `GotoPlayerAggressive`, `GotoRun`, `CombatGoto`, `TeleportReset`,
`SetHomePosition`, `KillObject`, `MoveIntoPurgatory` and ~70 more.

### The remaining blocker: actor binding

A verb acts on **its context's** actor, and the context is a
`VMInstanceInternal` (0x40 bytes; the vtable repeats at `+0x40`). There are ~879
of them for ~190 NPCs, and the actor's handle sits at **`+0x08`** - but it is
zero on most of them.

So instances are **per-running-script, not per-actor**: an NPC only has one while
a script is executing on it. Roughly 1 NPC in 10 is reachable at any moment.
Commanding an arbitrary NPC requires binding an instance to it, which is
unsolved.

Note `KillObject` and other `{void}|Object` verbs may act on the *passed* object
rather than the context's actor, which would sidestep this entirely - untested.

## Things that do not work

* **Direct position writes on NPCs.** Ignored, exactly as for the player. The
  actor's `+0x24` is a copy; the motion object at `+0x50` owns it, and writing
  both still does nothing. Use the engine's own teleport verbs instead.
* **Loading a save does not spawn.** Saves restore NPCs rather than running the
  spawn path, so captures only come from `warp` or a genuine level transition.
* **Ragdoll.** There is none - no fields, classes or verbs. Deaths are animated,
  with `m_onDeathGib` / `m_allowOnDeathGibFromBolts` / `m_onGibSpawnNPC` in
  `NPCPrefs` for gibbing. Adding ragdoll would mean writing a physics system,
  not enabling a feature.

## Instrumentation lessons

Every wrong conclusion during this work came from a broken measurement rather
than a wrong theory. Recorded so the same traps are not re-set:

* scan buffers capped at 64 reported exactly "64" and read as a real count
* NPC positions were read at `+0x44`; the actual field is `+0x24` (`PF_POS`),
  so every distance was noise - including "the spawn is right next to you"
* "the new NPCs are the last N entries" - the scan is ordered by address, not
  creation, so this sampled unrelated NPCs. Diff the before/after sets.
* the hook's hit counter only logged its first 3 hits and never reset, so it
  saturated on the session's first load and made runtime spawns invisible
* scanning `0x400` into a `0x40`-byte object produced matches belonging to
  fifteen neighbours
* the harvested type list accumulated across levels while being described as
  "this level", making the `dupetype` safety check unsound

When a result is surprising, check the instrument before theorising.
