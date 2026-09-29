// SWSE live prefs editor - read and write ANY loaded prefs record by name.
//
// Everything the game tunes - ammo knockback and damage, NPC health, weapon
// timing, motion, explosions - lives in prefs records (/data/prefs/**.txt)
// that are loaded into ordinary C++ objects. Offline tools reach them by
// patching the serialized records inside the .smb bundles on disk. This reaches
// the live objects instead: nothing on disk changes, nothing needs a backup,
// and the game's own files are never touched.
//
// FINDING AN OBJECT
//   /data/prefs/weapons/damagearmadillo.txt   a prefs path (hashed with the
//                                            game's own hasher)
//   7AE0C662                                  a path hash
//   @NPCWeaponPrefs                           every live object of an RTTI class
//   &1A2B3C40                                 one object by address
// A prefs object carries its own path hash at +0x0C; a hash is found by
// scanning the heap for it there, behind a valid RTTI vtable.
//
// NAMING A FIELD
//   m_maxKnockSpeed          a reflected field name (swse/reflect_gen.h, dumped
//                            from the game's reflection registration)
//   NPCPrefs::m_health       class-qualified, when a name is ambiguous
//   0x448                    a raw byte offset
//   ...:i  :b  :h  :f        type suffix - int, byte, hash, float (default)
//
// prefs.txt (any mod, ADDITIVE) - applied once per level, on a worker thread:
//   <target>  <field>  <value>
//   /data/prefs/weapons/damagedynamite.txt  m_maxKnockSpeedPlayer  25
//
// Every original value is captured before the first write, so `prefs restore`
// and switching the feature off put the shipped numbers back.
#pragma once

// (Re)read every enabled mod's prefs.txt. Returns entries parsed.
int  SWSE_PrefsEditLoad(char* msg, int msgLen);

// Per frame (feature `prefsedit` on): once per level, after a settle delay,
// apply prefs.txt on a worker thread.
void SWSE_PrefsEditTick();

// Apply again on the next tick, without waiting for a level load.
void SWSE_PrefsEditKick();

// Put back every value the editor has changed, from the file or the console.
int  SWSE_PrefsEditRestoreAll(char* msg, int msgLen);

// The `prefs` console command (find/get/set/dump/fields/class/restore/...).
void SWSE_PrefsCommand(int argc, char** argv, void (*emit)(const char*));

// Programmatic single write, used by shortcut commands such as `knockback`.
// target/field/value use the same syntax as prefs.txt. Returns objects written
// (0 = not loaded in this level), or -1 when the target, field or value is
// rejected - callers keep a value in prefs.txt only when this is >= 0.
int  SWSE_PrefsSet(const char* target, const char* field, const char* value,
                   void (*emit)(const char*));

// The same for many targets in ONE heap pass (a pass per target would stall
// the frame for seconds). Returns objects written, or -1 as above.
int  SWSE_PrefsSetMany(const char* const* targets, int n, const char* field,
                       const char* value, void (*emit)(const char*));

// Read one float field from many targets in one pass. found[i] = 0 when the
// target is not loaded. Returns how many were found.
int  SWSE_PrefsGetMany(const char* const* targets, int n, const char* field,
                       float* out, int* found);

// Append `target field value` to SWSE Console\prefs.txt so it is re-applied on
// every level (with the prefsedit feature on). Returns true on success.
bool SWSE_PrefsKeep(const char* target, const char* field, const char* value,
                    char* msg, int msgLen);

// The game's resource registry, read-only: the loaded resource (prefs object)
// whose path hash is `key`, or 0 when none is loaded. No call into the game, no
// reference count touched, nothing created.
unsigned SWSE_ResourceLookup(unsigned key);
// 1 = the registry was readable on the last lookup, 0 = not, -1 = never tried.
int      SWSE_ResourceRegistryOk();
// Is obj an instance of RTTI class `cls`, directly or by inheritance?
bool     SWSE_ObjIsA(unsigned obj, const char* cls);

// For the self-test: edits loaded, and values changed from shipped right now.
void SWSE_PrefsEditStats(int* edits, int* changed);
