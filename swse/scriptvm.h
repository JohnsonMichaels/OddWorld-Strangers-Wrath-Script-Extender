// SWSE Script-VM bridge: call the game's native .foo script handlers from
// SWSE Console. We capture a live ScriptContext by hooking a handler the
// game calls during normal play (GiveAmmo), then replay that context into any
// handler. See swse/research/SCRIPT_VM.md for the RE.
#pragma once

// Install the capture hook (call once, GL/first-frame time is fine).
void SWSE_ScriptVMInit();

// True once we have a live ScriptContext. No longer requires the ammo dance:
// this auto-primes by scanning for the context's vtable if none is held.
bool SWSE_ScriptHaveCtx();

// Find a live ScriptContext by scanning memory for its vtable, instead of
// waiting to steal one from a GiveAmmo call. 1 = found, 0 = none yet.
int  SWSE_AutoPrime();

// Run a named console action. Returns:
//   1  = executed
//   0  = no context captured yet (tell the user to grab ammo once)
//  -1  = unknown action
//  -2  = faulted (SEH) - action auto-disabled
int SWSE_ScriptDo(const char* action, int arg);

// Call ANY callable-now script function by name (generic dispatcher).
//   1 = ok, 0 = no context, -1 = not found, -2 = faulted.
int SWSE_ScriptCallByName(const char* name, int argc, char** argv);

// List function names containing `filter` (case-insensitive) into out[].
// Returns how many were written (up to maxOut).
int SWSE_ScriptList(const char* filter, const char** out, int maxOut);

// Prefix match (for Tab completion). Returns count written.
int SWSE_ScriptComplete(const char* prefix, const char** out, int maxOut);

// Arg-format string for a function ("", "i", "if", "e"...), or nullptr if
// unknown. i=int f=float b=bool e=enum(int).
const char* SWSE_ScriptArgs(const char* name);

// Toggle god mode. Returns 1 (now ON), 2 (now OFF), or 0 (no context yet).
int SWSE_ScriptToggleGod();

// Dump the captured player-context object to swse_log.txt (for RE of position
// / jump fields).
void SWSE_ScriptDumpContext();

// Log ctx's vtable pointer and check it against known RTTI classes (e.g.
// PlayerImpl) - tells us definitively what kind of object we're holding.
void SWSE_ScriptCtxInfo();

// Search the probe/watch object graph for a known exact int value - a
// built-in Cheat-Engine-style exact-value scan, scoped to what's reachable
// from ctx. Logs every [+selfOff]+subOff match to swse_log.txt.
void SWSE_ScriptFind(int value);

// Call virtual method #slot on ctx's OWN vtable, with up to 2 int args.
// Only meaningful once ctxinfo has told us what ctx's class/vtable is.
// Returns 1 ok (logs the method's return value), 0 no context, -2 fault.
int SWSE_ScriptVCall(int slot, int argc, int a0, int a1);

// Poke/freeze any field using the SAME [+selfOff]+subOff addressing shown in
// probe/watch log output - a built-in Cheat-Engine-style value editor.
//   SWSE_Poke: write once.  1 ok, 0 no context/field, -2 fault.
//   SWSE_Freeze: write every frame until unfrozen. Same return codes, -1 = full.
//   SWSE_Unfreeze / SWSE_UnfreezeAll: stop forcing a field (or all of them).
int SWSE_Poke(int selfOff, int subOff, int value);
int SWSE_Freeze(int selfOff, int subOff, int value);
int SWSE_Unfreeze(int selfOff, int subOff);
void SWSE_UnfreezeAll();

// Watch mode: auto-captures every frame for durationMs, then logs every
// field that changed at ALL during that window. No manual timing needed -
// start it, then do the in-game action at your own pace. `label` (optional,
// may be null/empty) is stamped into the log header/footer so multiple
// watch runs are easy to tell apart when scrolling back through the log.
// Returns 1 ok, 0 no context, -2 fault.
int SWSE_ScriptWatchStart(int durationMs, const char* label);

// Position access (teleport / vertical launch). Returns 1 ok, 0 no
// context/pos, -2 fault, 3 nothing-saved (restore only), -1 bad axis.
int SWSE_PosGet(float* out3);     // out3 may be null
int SWSE_PosSave();
int SWSE_PosRestore();
int SWSE_PosNudge(int axis, float delta);

// ---- heap diff scanner: locate the artifact inventory in memory ----------
// Store purchases bypass the script VM, so artifacts must be written
// directly. Snapshot memory, buy one artifact, then diff to see what flipped.
int SWSE_ScanStart();                 // returns regions snapshotted
int SWSE_ScanDiff(int maxReport);     // returns candidates found (logged)
// Narrow the candidate list. mode=1 keep those that changed again (after
// buying another artifact); mode=0 keep those that held steady (after doing
// nothing) - that removes timers/counters.
int SWSE_ScanRefine(int mode);
// Cleaner signal: snapshot while holding artifacts, run takeallartifacts,
// then this finds everything that got wiped to 0 - i.e. the inventory.
int SWSE_ScanCleared(int maxReport);
// Write a value to every tracked candidate at once (grant everything).
int SWSE_ScanPokeAll(int value);

// Call the same getter TakeAllArtifacts uses (0x4588A0) and dump the object
// graph it returns - locates the inventory directly instead of scanning.
int SWSE_DumpInventory();

// Dump arbitrary memory as dwords (walk the inventory structure by hand).
int SWSE_DumpAddr(unsigned addr, int dwords);

// ---- Script spy: watch the game call its own script functions ------------
// Hooks a set of interesting handlers and logs each call with the arguments
// the interpreter actually passed (read via ctx->vtable[0x74]). Nothing is
// modified - this is how we learn the real calling format.
int  SWSE_SpyStart(int budget);   // returns hooks installed
void SWSE_SpyStop();              // stops logging, prints hit counts

// ---- Hardware watchpoint ("what writes to this address?") ----------------
// Store purchases bypass the script VM, so the artifact-adding code is plain
// C++. A debug-register watchpoint catches whatever writes the inventory head.
// ---- Player fields (via the getter chain - no priming, no Cheat Engine) --
// invdump located health at player+0x78 (three floats: current/max/base,
// reading 300 on normal) and stamina at +0x8C. Writing all three makes the
// change stick instead of being clamped back to the old maximum.
int SWSE_PlayerHealth(float* cur, float* mx, float* base);
int SWSE_PlayerSetHealth(float v);
int SWSE_PlayerStamina(float* cur, float* mx, float* base);
int SWSE_PlayerSetStamina(float v);
int SWSE_PlayerGet(int off, float* a, float* b, float* c);
int SWSE_PlayerSet(int off, float v);

int  SWSE_WatchWrite(unsigned addr);   // 1 ok, 0 bad addr, -1 failed
int  SWSE_WatchInventory();            // resolves player+0x1C and watches it
int  SWSE_WatchExec(unsigned rva, int once);  // break when this CODE runs.
// once=1 captures a single hit then disarms from inside the handler. Required
// for anything on a per-character/per-frame path (e.g. the Granny pose builder),
// where a free-running breakpoint wedges the game.
int  SWSE_WatchRW(unsigned addr, int len);  // break when this field is READ too
void SWSE_WatchOff();
int SWSE_PokeAddr(unsigned addr, int value);   // 1 ok, 0 bad addr, -2 fault

// ---- store grant path (the one that actually works) -----------------------
// Calls the game's own item-grant function (RVA 0x87C80), the one the store
// uses after deducting moolah. Bypasses the script VM entirely - GiveArtifact
// needs an ArtifactPref and faults, this doesn't.
//   `name` may be a bare artifact name ("surgerybid") or a full prefs path.
//   1 = called, 0 = no wallet (no save loaded), -2 = faulted.
int   SWSE_GrantItem(const char* name, int qty, char* msg, int msgLen);
void* SWSE_WalletObj();               // resolved live; survives save reloads
int   SWSE_Moolah(float* out);        // 1 ok, 0 no wallet, -2 fault
int   SWSE_SetMoolah(float v);

// Hook 0x87C80 and log the exact item-name string a real store purchase
// passes. Our guessed prefs path faulted, so this reads the true format
// instead of guessing. 1 = ok, -1 = could not install.
int   SWSE_GrantSpy(int on);

// Give a weapon by prefs name ("crossbowupgrade") via WeaponPref+GiveCrossbow.
// Weapons have no artifacts/ token, so the store grant path cannot reach them.
// 1 = called, 0 = no context, -2 = faulted.
int   SWSE_GiveWeapon(const char* name, char* msg, int msgLen);

// Warp to another level via the game's own LoadLevel / LevelTransition.
// Levels are lm_level_00 .. lm_level_06 (plus lm_level_02a). Also the only way
// to reach level-scoped prefs such as region_04's crossbowupgrade.
int   SWSE_LoadLevel(const char* name, bool transition, char* msg, int msgLen);

// Motion prefs, using offsets recovered from the game's own reflection data
// (swse/research/FIELD_OFFSETS.tsv). Instances are found by RTTI vtable scan.
//   field: 0 = jump height, 1 = run speed, 2 = gravity, 3 = air control
//   set   : non-null to write; cur receives the current value
// Returns the number of instances touched (0 = none found).
int   SWSE_MotionField(int field, const float* set, float* cur);

// Make the level's ActorSpawner objects fire. There is no script verb that
// spawns an actor, but a spawner's own tick will do it once it is enabled,
// its counter cleared and its interval zeroed. count = new cap; 0 = just count.
int   SWSE_Spawn(int count);

// Move a spawner to the player's position and arm it, so actors appear next to
// us rather than wherever the level placed the spawner.
// 1 = ok, 0 = no player position, -1 = no spawner found.
int   SWSE_SpawnHere(int count);

// Enable the level's critter paths and make them spawn quickly.
// Unlike ActorSpawner (whose tick never runs), this is the mechanism the
// game's own SetCritterPathSpawnEnabled drives: one byte at CritterPath+0x60,
// plus the reflected CritterPathPrefs fields.
// count = 0 just reports. Returns paths touched; *prefsOut gets prefs touched.
int   SWSE_Critters(int count, int* prefsOut);

// NPC factory (0x25A50): hook it to capture a real creation's arguments, then
// replay them. Nine of its arguments point into level tag structures, so
// replaying a genuine call beats synthesising one -- the approach that worked
// for artifacts. 1 = ok, 0 = nothing captured, -2 = faulted.
// The whole spawn routine, 0x184D90 SpawnNPCFromTag(this=tag, arg0) -- create,
// init, position, register. Replaying only the factory left registration
// faulting on a null the skipped init calls would have filled in.
int   SWSE_FindGeomInst(unsigned* out, int maxOut);  // spawn anchors (WHERE)
int   SWSE_FindNpcPrefs(unsigned* out, int maxOut);  // spawnable NPC TYPES
int   SWSE_FindNpcs(unsigned* out, int maxOut);  // live NPCs (heap, self-ptr checked)

// Incremental form of the above. The full walk costs 150-270 ms on the render
// thread, which is a visible freeze; this spends at most budgetMs per call and
// sets *complete when a whole pass has finished. Returns -1 while still in
// progress, otherwise the number of actors written.
int   SWSE_FindNpcsStep(unsigned* out, int maxOut, double budgetMs, int* complete);

// Re-check an actor list already held, compacting out dead entries. Returns
// the surviving count, or -1 if it could not be read (rebuild from scratch).
// Far cheaper than rediscovery: ~260 reads instead of a 768 MB walk.
int   SWSE_ValidateNpcs(unsigned* list, int n);

// Promote a percentage of the live NPCs of a type into elites, by writing
// health on individual actors rather than on the shared prefs. hash 0 = any
// type. Type-level commands convert every NPC at once; an encounter wants the
// occasional dangerous one.
int   SWSE_MakeElites(unsigned hash, int percent, float health, char* msg, int msgLen);
int   SWSE_NpcTags(unsigned* out, int maxOut);   // live NPCTags in this level
// Build a fresh NPCTag (0x184C20), point it at a chosen NPCPrefs and the
// player's position, and run the game's spawn routine. No capture needed.
int   SWSE_SpawnNpc(int typeIndex, char* msg, int msgLen);
// Clone a captured spawn record (correct observed layout) and retarget it.
int   SWSE_SpawnCloned(char* msg, int msgLen);
int   SWSE_NpcRoutineSpy(int on);
// Spawn using only the game's own routine and its own live arguments.
// Run constructed-object spawns DURING the next level load, to separate
// "objects we built" from "outside a load" as the reason npcnow is invisible.
int   SWSE_NpcBuildTest(int n);
// Retype the game's own spawn tags during a load: pick the character without
// constructing anything. 0 disables.
int   SWSE_DupeType(unsigned hash);
// Retype only every Nth spawn -- a whole level of bosses crashes the game.
int   SWSE_DupeEvery(int n);
// Raw hook hit count -- the honest measure of how often the spawn routine ran.
int   SWSE_NpcHits();
int   SWSE_NpcDupe(int n);
int   SWSE_NpcCount(int n);
// Replay the captured call outside a level load, to test whether the tag
// survives. 1 = called, 0 = tag is dead (message says how), -2 = faulted.
int   SWSE_NpcReplay(char* msg, int msgLen);
// Spawn now, using a live NPCTag and a live GeometryInst. Self-verifying:
// reports the NPC count before and after. 1 = count went up, 0 = it didn't.
int   SWSE_SpawnNow(int count, int geomIndex, char* msg, int msgLen);
// Report the guard byte SpawnNPCFromTag checks before doing any work.
int   SWSE_SpawnGate(char* msg, int msgLen);
// Type hashes harvested from a level load -- the menu npcnow's type arg indexes.
// Identify the NPC nearest the player and which spawn type produces it --
// "give me more of what I'm looking at", without needing a name for it.
// Move live NPCs to the player. This engine creates no NPCs at runtime, so
// relocating existing ones is what "summon" can actually mean here.
int   SWSE_BringNpcs(int count, unsigned typeHash, char* msg, int msgLen);

// Relocate NPCs to an arbitrary point (triggers use this for ambushes).
// The engine never creates NPCs at runtime, so this MOVES existing ones; a 0
// return means the level had nobody left to move.
int   SWSE_BringNpcsTo(float x, float y, float z,
                       int count, unsigned typeHash, char* msg, int msgLen);

// Live NPCs of a type, from the cached actor list. -1 = cache not populated.
int   SWSE_CountNpcsOfType(unsigned typeHash);

// ---- reserve pool: spare NPCs for ambushes --------------------------------
// The engine only creates NPCs at level load, so a cleared area has nobody
// left to ambush with. Build asks the level for extra cast (takes effect on
// the next load); Park banks the distant ones below the map so spawnat can
// draw on them.
int   SWSE_ReserveBuild(int extraPerTag, char* msg, int msgLen);
int   SWSE_ReservePark(char* msg, int msgLen);
int   SWSE_PlayerTeleport(float x, float y, float z);
// Send NPCs at the player via the game's own AI (GotoPlayerAggressive). Needs
// Object arguments, which are handles -- {u16 index, u16 generation} into the
// table at 0x9D55F0 -- not pointers.
int   SWSE_SendNpcs(int count, unsigned typeHash, char* msg, int msgLen);
// Order NPCs of one type to attack one of another, through the game's own AI.
// The engine has no NPC-vs-NPC hostility, so this is commanded rather than set.
int   SWSE_MakeAttack(unsigned attackerHash, unsigned victimHash, int count,
                      int useRun, char* msg, int msgLen);
// Locate the field holding "who I am fighting", by searching an aggro'd NPC
// for the player's own handle and pointer.
int   SWSE_FindTarget(unsigned npc, char* msg, int msgLen);

// Raid mode: standing hostility. Re-points every active raider at the nearest
// valid enemy every few frames -- townsfolk OR the player, whichever is closer,
// so raiders that lose a victim come for you instead of giving up.
// Who is fighting whom right now, read from every active NPC's Mind.
int   SWSE_ScanTargets(char* msg, int msgLen);
// Repeatedly tell two NPCs that the other hurt them -- the retaliation route,
// which is the one path to hostility the AI accepts when the player uses it.
int   SWSE_Feud(unsigned typeA, unsigned typeB, int rounds, char* msg, int msgLen);
int   SWSE_RaidMode(unsigned attacker, unsigned victim, int on, int everyFrames);
// Decoy mode: leave the target as the player but feed the AI the victim's
// position, so it fires at the spot the victim is standing in. Works around the
// blanket refusal to target another NPC, using the fact that projectiles damage
// whatever they hit.
int   SWSE_DecoyMode(unsigned shooter, unsigned victim, int on, int everyFrames);
void  SWSE_DecoyTick();
int   SWSE_DecoyCount();

void  SWSE_RaidTick();          // called once per frame
int   SWSE_RaidCount();
bool  SWSE_RaidOn();
int   SWSE_NpcNear(char* msg, int msgLen);
// The game's own path hash (0x24D920), exposed so candidate names can be
// tested against harvested type hashes.
unsigned SWSE_HashPath(const char* path);
// Resolve a type hash through the game's own resolver, to tell "the hash is
// rejected" apart from "the type is read from a field we have not found".
int   SWSE_Resolve(unsigned hash, char* msg, int msgLen);

// ---- AI / combat tuning ---------------------------------------------------
// Per-character AI knobs: perception (sight distance, sixth sense, alert-state
// decay) and ranged-weapon timing (fire rate, reload, accuracy, miss time).
// These are ordinary prefs objects, present regardless of difficulty setting.
unsigned SWSE_AiPrefsOf(unsigned typeHash, unsigned* weapon, unsigned* npcPrefs);
int SWSE_AiDump(unsigned typeHash, void (*emit)(const char*));
int SWSE_AiSet(unsigned typeHash, int onWeapon, int offset, float value);

// Locate the perception prefs object by its memory SHAPE - four sight blocks
// at a 0xA4 stride. The hash resolver cannot reach it (it is NPCPrefs-only).
int SWSE_FindAiPrefs(unsigned* out, int max, double budgetMs);

// Join characters to the guns they carry: NPCPrefs.m_rangedWeapon matches
// NPCWeaponPrefs's own path hash at +0x0C. Health and kill bounty come along
// because research/NPC_TUNING.md indexes character NAMES by stock health.
struct NpcGunRow {
    unsigned npcHash, weaponHash, weaponAddr;
    float    health, killMoolah, fireRate;
};
int SWSE_NpcGuns(NpcGunRow* out, int max, double budgetMs);

// Locate NPCWeaponPrefs by shape. READ ONLY - the signature is weaker than
// the perception one, so callers must not write through these addresses.
int SWSE_FindWeaponPrefs(unsigned* out, int max, double budgetMs);
// Spawn ring radius in tenths of a unit, tunable at runtime.
int   SWSE_SetSpawnRadius(int tenths);
void     SWSE_ClearNpcTypes();
int      SWSE_NpcTypeCount();
unsigned SWSE_NpcTypeHash(int i);
// Name any live object's class via runtime RTTI. General-purpose: the static
// RTTI dump has gaps, so this is the reliable way to identify an address.
int   SWSE_WhatIs(unsigned addr, char* msg, int msgLen);
// Every live object of the same class as addr.
int   SWSE_Instances(unsigned addr, char* msg, int msgLen);
// Every named object near the player, whatever its class -- "what is that?"
int   SWSE_Nearby(int radius, char* msg, int msgLen);
// Compare two live objects, reporting only the fields that differ.
int   SWSE_DiffObjects(unsigned a, unsigned b, int len, char* msg, int msgLen);
unsigned SWSE_FirstNpcOfType(unsigned hash);
// Set a character type's health (prefs + every live NPC of that type).
// Townsfolk immortality is just m_health = 100000; this is the cure.
int   SWSE_SetTypeHealth(unsigned hash, float health, char* msg, int msgLen);
// Make a character type gib on death rather than play a death animation.
int   SWSE_SetTypeGib(unsigned hash, int on, char* msg, int msgLen);
// m_hurtReaction: 0 = staggers/knocked about, 2 = unflinchable.
int   SWSE_SetTypeHurt(unsigned hash, int value, char* msg, int msgLen);
// m_affGenerally: affiliation. Outlaws and townsfolk both ship as 1, so they
// never fight each other -- the lever for town raids.
int   SWSE_SetTypeAff(unsigned hash, int value, char* msg, int msgLen);
// Sweep every character type the current level uses. -1 leaves a setting alone.
int   SWSE_SetAllTypes(float health, int gib, char* msg, int msgLen);
// Load characters.txt: per-character health/gib applied automatically as each
// level spawns them, so a mod is a file rather than commands typed each load.
int   SWSE_LoadTuning(const char* path, char* msg, int msgLen);
// settings.txt: named feature toggles (noimmortals, immortalsgib, ...).
// Rules rather than hash lists, so they catch characters nobody has named.
int   SWSE_LoadSettings(const char* path, char* msg, int msgLen);
// A character type's current health / gib flag, for auditing a level's cast.
int   SWSE_TypeInfo(unsigned hash, int* hpOut, int* gibOut);
// Also reports m_hurtReaction and m_affGenerally, for mapping a level's factions.
int   SWSE_TypeInfo2(unsigned hash, int* hpOut, int* gibOut, int* hurtOut, int* affOut);
// Locate every live NPC of a type, with the distance to the nearest.
int   SWSE_WhereIs(unsigned hash, char* msg, int msgLen);
// The town panic system -- alarm bells, fleeing townsfolk, turret activation.
// forever/radius < 0 just reports the current values.
int   SWSE_TownPanic(int forever, int radius, char* msg, int msgLen);
// The game's own alarm verbs. A raid needs a reason for the town to panic, not
// NPC-vs-NPC hostility (which this engine does not have).
int   SWSE_PostAlarm(unsigned zoneId, int useBell, char* msg, int msgLen);
int   SWSE_PanicZones(unsigned* out, int maxOut);
int   SWSE_FindByVtable(unsigned vt, unsigned* out, int maxOut);
int   SWSE_NpcRoutineRun(unsigned tagOverride, char* msg, int msgLen, int typeIndex);

int   SWSE_NpcSpy(int on);
int   SWSE_NpcLast(char* msg, int msgLen);
// atPlayer: patch the player's coordinates into the spawn transform first.
int   SWSE_NpcLastAt(char* msg, int msgLen, bool atPlayer);

// Replay a grant call captured by the spy, using the game's own entry object.
// Isolates "my call is wrong" from "my synthetic entry is wrong".
int   SWSE_GrantLast(int qty, char* msg, int msgLen);

// ---- exact-value search ---------------------------------------------------
// Diff scans were unusably noisy. An exact match on a number the player can
// see (moolah) is precise. Find moolah -> watch it -> buy -> the watchpoint
// lands inside the purchase code, right next to the add-item call.
int      SWSE_FindValue(int value, int maxHits);  // hits found (logged)
int      SWSE_FindNarrow(int value);              // keep hits now == value
unsigned SWSE_FindHit(int i);                     // address of hit i, 0 if none

// Give an artifact by short name ("breederbag") or full prefs path. Handles
// the String->ArtifactPref conversion and verifies via HasArtifactCount.
// Returns 1 = verified, 2 = called but unverified, 0 = no context, -2 = fault.
// `msg` receives a human-readable result for the console.
int SWSE_GiveArtifact(const char* name, char* msg, int msgLen);

// ---- Pointer chains (Cheat-Engine style, no script context needed) -------
// Chains are defined in SWSEMods\SWSE Console\pointers.txt as:
//     name = type(f|i), baseRVA, off1, off2, ...     (all hex except type)
// They resolve from the module base every time, so they survive restarts and
// need no priming. This is the general mechanism for turning any CE find into
// a permanent SWSE command.
int  SWSE_PtrLoad();                       // (re)load pointers.txt, returns count
int  SWSE_PtrCount();
const char* SWSE_PtrName(int i);
char SWSE_PtrType(int i);                  // 'f' or 'i'
int  SWSE_PtrFind(const char* name);       // index or -1
bool SWSE_PtrRead(int i, float* fOut, int* iOut, char* trace, int traceLen);
bool SWSE_PtrWrite(int i, float fVal, int iVal);
bool SWSE_PtrSetFrozen(int i, bool on, float fVal, int iVal);

// Point probe/watch at a specific address (e.g. a resolved pointer chain that
// lands in the player object) instead of the ScriptContext, which holds no
// gameplay state at all. Pass nullptr to go back to the context.
void SWSE_SetWatchAnchor(void* p);

// Resolve a chain by index and return its final address (nullptr if it can't
// resolve). Used to anchor watch/probe onto real player memory.
void* SWSE_PtrResolve(int i);

// Call once per frame - applies god mode (re-max health) while enabled.
void SWSE_ScriptTick();

unsigned SWSE_LastNearNpc();

// Animation probe: call a (Object) or (Object,int) animation verb on the NPC
// nearest the player. 'which' is torso|endtorso|stop. The int's VM type tag is
// still unknown, so it is a parameter rather than a constant.
int SWSE_AnimVerb(const char* which, int intArg, int tag, char* msg, int msgLen);
