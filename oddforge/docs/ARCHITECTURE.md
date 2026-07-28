# SWSE Architecture - two products under one roof

SWSE has two halves, matching the two ways you can change a game:

```
┌────────────────────────────────────────────────────────────────┐
│  SWSE                                                        │
│                                                                  │
│  ┌──────────────────────┐      ┌────────────────────────────┐   │
│  │  SANDBOX             │      │  MOD LOADER                │   │
│  │  (authoring)         │      │  (distribution + runtime)  │   │
│  │                      │      │                            │   │
│  │  the Studio GUI:     │      │  SWSEMods/ folder      │   │
│  │  edit textures,      │─────▶│  load_order.txt            │   │
│  │  values, scripts;    │ mods │  merge + rebuild archives  │   │
│  │  export a mod folder │      │  backup / restore vanilla  │   │
│  └──────────────────────┘      └────────────────────────────┘   │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  SCRIPT EXTENDER  (SWSE - Stranger's Wrath Script Extender)        │ │
│  │  a DLL injected into stranger.exe that adds NEW engine     │ │
│  │  capabilities mods can call - the deep track               │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

## Two tiers of mod capability - be honest about which is which

Not every mod idea needs the same machinery. Sorting them correctly is what
keeps the project realistic.

### Tier 1 - Data/script mods (the Mod Loader handles these, no code injection)

These change *data the engine already reads*: archives, the `.foo` script VM,
effect-config records. The Mod Loader ships them. Achievable now / soon:

- **Texture packs, reskins** - done (DXT1 replace).
- **Stat rebalances** - weapon damage, bounty payouts, melee damage, health
  values - via the Values system.
- **New/edited NPC behavior *within the existing script vocabulary*** - the
  `.foo` language already has `OnDeath`, `OnBounty`, `OnCombat`, `GetHealth`,
  `Set`, `StartScript`, `MoveIntoPurgatory`… A mod can rewrite an NPC's event
  handlers using those verbs. That covers a LOT of "new behavior."
- **Effect/gore tuning** - the `EffectMixDef\char_*_hit` records and effect
  configs control hit reactions. Blood-decal *intensity*, existing-effect
  swaps, and spawn counts are likely data-reachable.

### Tier 2 - Engine-extending mods (need the Script Extender / native hooks)

These require *new engine behavior that no data file can express*:

- **Dismemberment** - if the engine has no "detach limb" capability, no data
  edit creates one. Needs a native hook into the damage/skeleton code.
- **New blood-decal *systems*** (persistent decals, new projection) if the
  engine caps or lacks them.
- **New script functions** - e.g. giving `.foo` a `SpawnGib()` or
  `SetPlayerJumpHeight()` call the original VM never had.
- **Player movement tuning** (jump height, run speed) if, as evidence
  suggests, those live in the exe rather than in a prefs record.

## What a Script Extender actually is (and the plan)

A script extender = a DLL that loads into the game's process at startup and
patches the running engine in memory, exposing new hooks/functions to mods.
(Precedent: SKSE for Skyrim, OBSE, F4SE, MWSE.)

**Injection method for a DX9 game like Stranger's Wrath:** a *proxy DLL*. The
game already loads system DLLs (`d3d9.dll`, `dinput8.dll`, `winmm.dll`) from its
own folder before the system ones. We drop a same-named DLL that (a) forwards
every real export to the true system DLL, and (b) on load, runs our init:
install hooks, load SWSE mods' native plugins, extend the script VM.

**The hard, honest part - this is real reverse engineering:**
1. Load `stranger.exe` in Ghidra/IDA, locate the `.foo` script-function
   registration (where `GetHealth`, `Set`, etc. get bound to native code).
2. Locate damage application, decal spawning, skeleton/bone update.
3. Write hooks (MinHook/Detours) that add our behavior.
Each capability is its own research spike. Some may be quick; some may be walls.
This is a multi-stage effort with uncertainty, not a weekend feature.

**Sequencing decision:** build the **Mod Loader first** (ships Tier-1 mods to
real users now, creates the community), then pursue **SWSE** to unlock Tier-2.
The loader is also what will *distribute* SWSE plugins when they exist.
