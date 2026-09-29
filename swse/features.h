// SWSE feature switches - the on/off panel SWSE Setup ticks.
//
// SWSE is several independent systems living in one DLL: the graphics
// pipeline, the console, additive hit reactions, foliage wind, AI tuning, HD
// texture replacement. A player who wants HD textures should not have to take
// the graphics pipeline with them, and a modder debugging one system wants the
// others out of the way.
//
// Read once at startup from SWSEMods\features.txt.
//
// CONSOLE-ONLY BY DEFAULT (1.1). Only the console is on unless features.txt
// says otherwise - a missing file, or a missing line, means OFF for every
// other system. Nothing is removed: every system is still compiled in and
// comes back with one line in features.txt, or live with `features <name> on`.
// 1.0.x defaulted everything ON; a clean install now leaves the game exactly
// as shipped apart from the console.
//
// AUTO (1.1, aituning and playertune only). These two switches also take
// `auto`, their default: the system runs only while its own file asks for
// something - an `active = <profile>` line in aiprefs.txt, or any value in
// playerprefs.txt - and stays off otherwise. A stock install asks for
// nothing, so it is still console-only; a tool that writes those files
// (Stranger: Armed to the Teeth does) gets its rules applied without also
// having to edit features.txt. An explicit `on` or `off` always wins.
//
// A feature that is off at launch is not merely inert - its hooks are never
// installed. That matters: "off" should mean the game runs as if SWSE never
// touched it, not that a hook runs and returns early. Turning one on later
// installs its hooks at that point; turning one off live stops its effect
// (restoring shipped values where the system keeps a baseline), but a hook that
// was already installed stays in place, inert, until the next launch.
//
// PLUGINS (1.1). Every native plugin found under SWSEMods\<Mod>\plugins\ is a
// switch of its own, named after its DLL, added after the built-ins (ids
// FEAT_COUNT and up) when plugins.cpp discovers it - before any of its code
// could run, which is the point: the switch must exist while the DLL is still
// unloaded. Its value is whatever features.txt holds for that name (kept from
// the first read even though no plugin existed then); with no line it is off.
// A plugin can never take a built-in's name or nickname, and a plugin switch
// is never `auto`: a DLL runs only when the user names it.
#pragma once

// A fixed underlying type, so ids past FEAT_COUNT (plugin switches) are legal
// values of the enum.
enum SwseFeature : int {
    FEAT_CONSOLE = 0,   // in-game console overlay + remote mailbox
    FEAT_GRAPHICS,      // post-process pipeline (RTGI etc.)
    FEAT_HDTEXTURES,    // .oft texture replacement at upload
    FEAT_HITREACT,      // additive hit reactions
    FEAT_FOLIAGE,       // foliage identification + wind (grass, plants)
    FEAT_AITUNING,      // aiprefs.txt NPC tuning
    FEAT_TRIGGERS,      // triggers.txt mod-defined game events
    FEAT_NPCTUNING,     // characters.txt + console.txt, applied every level load
    FEAT_PLAYERTUNE,    // playerprefs.txt player health/stamina/motion
    FEAT_PREFSEDIT,     // prefs.txt live edits to any loaded prefs record
    FEAT_RAYTRACE,      // ray-traced AO in the graphics pipeline (EXPERIMENTAL)
    FEAT_COUNT
};

// Load SWSEMods\features.txt. Safe to call more than once; only the first
// call reads the file.
void SWSE_FeaturesInit();

// Is a feature enabled? Before Init runs this answers with the DEFAULT (only
// the console on), so an initialisation-order mistake can never switch a
// system on that the user did not ask for.
bool SWSE_Feature(SwseFeature f);

// The value a feature takes when features.txt does not mention it.
bool SWSE_FeatureDefault(SwseFeature f);

// Human-readable name, for the console and the self-test.
const char* SWSE_FeatureName(SwseFeature f);

// One-line description of what the switch controls.
const char* SWSE_FeatureDescribe(SwseFeature f);

// Look a feature up by name. Accepts the canonical names plus a few obvious
// aliases (grass/wind -> foliage, hd -> hdtextures, gfx -> graphics, ...), and
// plugin switches and their nicknames. Canonical names are matched before any
// nickname. Returns -1 when nothing matches.
int SWSE_FeatureFind(const char* name);

// Flip a feature in memory. Does NOT start or stop the system - the frame hook
// owns that (SWSE_FeatureStart/Stop in framehook.h) - and does not persist.
void SWSE_FeatureSetFlag(SwseFeature f, bool on);

// `auto` (see AUTO above): may this feature take it, and does the file say so.
bool SWSE_FeatureAutoCapable(SwseFeature f);
bool SWSE_FeatureIsAuto(SwseFeature f);
// Record `auto` as the value features.txt should hold (SetSaved clears it).
void SWSE_FeatureSetSavedAuto(SwseFeature f);

// Record the value features.txt should hold for a feature. Kept apart from the
// live flag so a `temp` switch never leaks into the file when some OTHER
// feature is saved later.
void SWSE_FeatureSetSaved(SwseFeature f, bool on);
// ...and read it back.
bool SWSE_FeatureSaved(SwseFeature f);

// Write the SAVED switches back to features.txt, keeping every comment and
// unknown line; known keys are rewritten in place, missing ones appended.
bool SWSE_FeaturesSave(char* msg, int msgLen);

// True if features.txt existed (so the console can say "defaults" honestly).
bool SWSE_FeaturesFromFile();

// ---- plugin switches (1.1) ------------------------------------------------
#define SWSE_MAX_PLUGIN_FEATS 64

// Every switch: the built-ins plus the plugin switches added so far. Loops
// over features run to this, not to FEAT_COUNT.
int  SWSE_FeatureTotal();

// Add (or find again, on a rescan) the switch for plugin `name` - a valid
// features.txt key, lower case a-z 0-9 _ -. Returns its id, or -1 when the
// name is a built-in switch or nickname, invalid, or the table is full.
int  SWSE_FeatureAddPlugin(const char* name);

// What the plugin said about itself in Query: a description for `features`
// and features.txt, and comma-separated console nicknames. A nickname that
// is already a switch or another switch's nickname is dropped with a log line
// (built-ins first, then plugins in the order they came).
void SWSE_FeatureSetPluginInfo(int id, const char* desc, const char* aliases);

bool SWSE_FeatureIsPlugin(int id);

// True for a built-in switch name or nickname: names no plugin may take.
bool SWSE_FeatureNameReserved(const char* name);

// Each key=value line of features.txt that names no switch, with whether it
// says on. For the "switches on 'x', but no plugin by that name" log line.
void SWSE_FeaturesForEachUnclaimed(void (*fn)(const char* key, bool on, void* ctx), void* ctx);
