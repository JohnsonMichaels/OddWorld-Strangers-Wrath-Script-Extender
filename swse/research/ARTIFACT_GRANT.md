# Giving items without the script VM - the store grant path

**Status: working.** `grant <artifact> [qty]` adds any artifact anywhere, for
free, with no priming and no shopkeeper.

All RVAs are module-relative (runtime base was `0x990000` under ASLR; SWSE
resolves everything through `GetModuleHandleA(NULL) + RVA`).

## Why the script VM was a dead end

`GiveArtifact` is a registered script function, but it takes an `ArtifactPref`
object rather than a name or an id. The `String -> ArtifactPref` converter at
`0x168E50` does return a pref, yet the call faults on both calling conventions
we tried, and `HasArtifactCount` returns a pointer where a count is expected:

```
surgerybid: called but not verified
  [VM: pref=13FDAF34 count 342836080 -> 342836080 | mode A: FAULTED | mode B: FAULTED]
```

The script spy also proved that **buying from a store fires zero script
functions** - the entire store is C++. So the script VM was never going to be
the route.

## How the path was found

A hardware watchpoint on moolah. Moolah is a **float**, which is why an
integer-valued scan found only stack copies; searching for the float bit
pattern found the single heap address. Watching writes to it caught:

```
WATCH HIT: code at module+0x87B51 wrote to 13FFB11C
   new value = 47201B00   ecx=13FFB110
   bytes around EIP: 4D 10 D8 41 0C D9 59 0C | 83 7E 08 FF
```

`D8 41 0C` = `fadd [ecx+0Ch]`, `D9 59 0C` = `fstp [ecx+0Ch]` - the deduction.
Walking outward from there gave the whole chain.

## The chain

| RVA | Role |
| --- | --- |
| `0x87880` | store UI dispatcher; string-compares `"buy_item"`, `"category_changed"` |
| `0x87A20` | purchase: clamps quantity to affordable + in stock, deducts moolah, calls the grant |
| `0x87C80` | **the grant** - loops `qty` times |
| `0x1CD100` | resolves the entry into an item ref (bumps the name refcount) |
| `0x210890` | normalizes the path in place (`/` to `\`), reallocating if refcount != 1 |
| `0x1CE0F0` | gives one item |
| `0x24D920` | hash a path: `/` to `\`, `tolower`, table at `0x7F7478`, length mixed in last |
| `0x24D990` | hash a plain string (same table, no slash rewriting) |
| `0x588A0` | inventory getter; also the root of the wallet lookup |

## Calling the grant

```
0x87C80(ecx = quantity, arg0 = playerObj, arg1 = wallet, arg2 = itemEntry*)  ; ret 0xC
```

**`arg0` looks unused and is not.** It is never referenced in the main body, so
a disassembly that stops at the first `ret` (or a window ending a few bytes
short, as mine did) will say it can be null. But when the item's name is
already in the game's table, control takes the cache-hit branch at `0x87D67`:

```
0x87d67  mov ecx,[ebp+8]       ; arg0
0x87d73  mov edx,[ecx]         ; its vtable  <-- null deref if arg0 == 0
0x87d75  mov eax,[edx+0x230]
0x87d82  call eax
```

Pass the object the wallet was reached through - the wallet getter is slot
`+0x234` on that same class, so it is already in hand. Symptom of getting this
wrong: items the game has never seen work fine (they take the grant loop),
while anything encountered earlier in the playthrough faults.

```c
struct ItemEntry { NameObj* name; int price; int stock; };  // 12 bytes
```

`price` and `stock` are read by the *purchase* function, not the grant, so they
can be anything when calling `0x87C80` directly.

### NameObj - the field that cost us the most time

`0x210890` allocates exactly `0x10` bytes for one of these, which pins the
layout:

```c
struct NameObj {
    const char* data;      // +0x00
    int         pad04;
    int         length;    // +0x08  INCLUDING the terminator
    int         refcount;  // +0x0C
};
```

Two non-obvious requirements, each of which produced a different failure:

- **`+0x0C` refcount must exist.** `0x1CD100` does `add [P+0x0C], 1`. A 4-byte
  fake object put that write into the neighbouring static - an access
  violation. Keep the count high so the release path can never reach 0 and
  free a static we own.
- **`+0x08` length must be set.** `0x210890` does `mov ecx,[P+8]; dec ecx` and
  bails out when it is zero. With length 0 the name behaves as `""`, so the
  item is created but resolves to nothing: it appears in the bag as a blank
  "blob" with no icon. This is why changing the name format never helped -
  the string was empty in every attempt.

### Name format

`\data\prefs\artifacts\<name>.txt` - captured from a real purchase by hooking
`0x87C80` (`grantspy`). Backslashes are what the store passes. Forward slashes
hash identically (`0x24D920` rewrites them), but passing backslashes means
`0x210890` finds nothing to rewrite and never tries to reallocate our buffer.

### Wallet

Resolved the way `0x87A20` does, so it survives save reloads with no scanning:

```c
void* g = ((void*(*)())Rva(0x588A0))();      // and require [g+8] != 0
void* obj = *(void**)(*(void**)g);
void* wallet = ((void*(__thiscall*)(void*))(*(void***)obj)[0x234/4])(obj);
// moolah is a float at wallet+0x0C
```

## Two kinds of artifact

The 53 names cover two different things, and both grant through the same call -
the prefs decide which:

- **Bag items** (`binoculars`, `attracterchipmunk`, `mongoriverpass`, …) appear
  under ITEMS with an icon and description.
- **Upgrades** (`damagestingbee`, `endurancemaxboost1`, `damagedynamite`, …)
  apply a modifier and show nothing under ITEMS. `damagestingbee` raises sting
  bee capacity to 300, visible in the AMMO row.

An upgrade producing no visible item is correct behaviour, not a failure.

## Console commands

| Command | Notes |
| --- | --- |
| `grant <name> [qty]` | bare name or explicit path |
| `giveartifact <name> [qty]` | now routed through `grant` |
| `allartifacts` | all 53 |
| `moolah [value]` | reads/writes `wallet+0x0C`; no priming |
| `grantspy [off]` | hooks `0x87C80` and logs a real purchase's arguments |
| `grantlast [qty]` | replays a captured call - isolates a bad call from a bad entry |

## Prefs are level-scoped (why the crossbow upgrade "didn't work")

`/data/prefs/weapons/crossbowupgrade.txt` is referenced **only** by
`bundles/region_04/lm_level_04/lm_level_04_tgl.smb`, next to the native-village
cinematic that awards it:

```
/data/geometry/characters/steef/anims/SteefTakeArmor.gr2
/data/prefs/Artifacts/SteefArmorLight.txt
\data\prefs\weapons\crossbowupgrade.txt
```

`GiveCrossbow` hashes the name and *looks it up*; it does not load anything. In
any other level the lookup misses and the engine builds a weapon with no ammo
bindings - you keep the same model, both ammo slots read 0, and firing crashes.
Warping to level 04 makes the prefs resident and the same call produces the
real upgraded crossbow.

Nothing about the call was ever wrong. Note the failure mode: **the engine
validates and skips rather than erroring**, so "the call succeeded" proves very
little on its own - the same trap as the blank-name artifact blobs.

## Level warp

`LoadLevel(String)` (RVA `0x160700`) and `LevelTransition(String)`
(`0x160510`) take a String using the same tag-5 marshalling as `GiveCrossbow`.

The catch is a six-instruction validator at `0x1FD570`:

```
mov al,[ecx] ; cmp al,'/' ; je ok ; cmp al,'\' ; je ok ; xor al,al ; ret
```

The name must start with `/` or `\` - **a bare level name is silently
rejected** and the load is skipped with no error. Full paths work:

```
/data/bundles/region_04/lm_level_04.lvl
```

Levels are `lm_level_00`..`lm_level_06` plus `lm_level_02a`. A path that does
not exist hangs the game on a load screen (loading music, frozen frame), so
validate the file before calling. Console: `warp <level|0-6>`, `levels`.

## Position is not writable

Worth recording so nobody repeats it. `player+0x24` is an xyz triple that reads
correctly but is a **per-frame copy**: a watchpoint caught `module+0x21E0F2`
copying it from `[edi]`. That source (a motion object at `edi-0x50`, findable
via its `+0x4C` back-pointer to `player+8`) is *also* a copy - writes there
revert too. The real writer at `module+0x226A52` fires ~9650 times per step and
stores values computed in registers, i.e. physics integrates position every
frame rather than reading it from anywhere.

So `pos` (read) works; teleporting by poking memory does not. Use `warp` for
level changes. `Teleport` (`0x14E140`) takes an **ID** handle validated against
a pool at `0x9D55F4`, not a name, and no String->ID resolver exists, so
arbitrary marker teleports are not reachable from the console.

## Method notes worth reusing

- **Anchor on a number the player can see.** Exact-value search on moolah beat
  diff scanning outright; the diff scans produced 60-200 candidates of pure
  noise, and one variant crashed the game.
- **Check float encodings.** Searching `41187` found only stack copies. The
  heap address only appeared when searching `41187.0f` as a bit pattern.
- **Replay before you synthesize.** `grantlast` reusing the game's own entry
  proved the calling convention was right, which pointed at the object layout
  instead - the actual bug, and not where the guessing had been aimed.
- **Log the fault address, not "it faulted."** A module offset is a lookup; a
  boolean is another guess.
