// SWSE mod registry - the extension point.
//
// THE PROBLEM THIS SOLVES
//     Every subsystem used to hardcode its own folder: foliage.cpp opened
//     "SWSEMods\SWSE Wind\foliage.txt", glspy.cpp opened
//     "SWSEMods\SWSE HD\textures", and so on. That makes the shipped mods
//     privileged. A modder who wanted to add three plants had to EDIT OUR
//     FILE, which means their work is destroyed by an update, two such mods
//     conflict, and nothing they make is redistributable as a folder.
//
// THE RULE
//     A mod is any folder under SWSEMods\. What it does is decided by which
//     files it contains, not by its name and not by anything it has to
//     declare. Convention over configuration: drop `foliage.txt` in your
//     folder and your plants sway. The shipped mods are ordinary folders that
//     follow the same rule and get no special treatment.
//
// CONVENTIONAL FILES  (any mod may provide any of these)
//     foliage.txt        plants the wind moves          ADDITIVE
//     wind.txt           wind strength/behaviour        LAST WINS
//     aiprefs.txt        NPC AI tuning profiles         ADDITIVE
//     characters.txt     per-character health/gib       ADDITIVE
//     hitreact.txt       additive hit reactions         LAST WINS
//     pointers.txt       pointer chains for the console ADDITIVE
//     graphics.txt       post-process settings          LAST WINS
//     console.txt        console/tuning toggles         LAST WINS
//     textures\*.oft     HD texture replacements        ADDITIVE, later wins
//     scripts\*.txt      new console commands           ADDITIVE
//
// MERGE POLICY
//     ADDITIVE  - every enabled mod contributes; use SWSE_ForEachModFile.
//     LAST WINS - the last enabled mod in load order decides; use
//                 SWSE_FindModFile. Load order therefore means what it says.
//
// ENABLING
//     load_order.txt lists folder names, top loads first, "!name" disables.
//     A folder absent from that file is enabled and sorts after the listed
//     ones, so dropping in a mod works without editing anything.
#pragma once

// Scan SWSEMods\ and read load_order.txt. Safe to call repeatedly; only the
// first call does work unless SWSE_ModsReload is used.
void SWSE_ModsInit();

// Re-scan after the user adds a folder or edits load_order.txt.
int  SWSE_ModsReload();

// Enabled mods, in load order.
int         SWSE_ModCount();
const char* SWSE_ModName(int i);
const char* SWSE_ModPath(int i);          // absolute folder path

// Every enabled mod that provides `relative` (e.g. "foliage.txt" or
// "textures"), in load order. `fn` receives the absolute path. This is the
// ADDITIVE case, and the reason a mod can extend a list without editing it.
void SWSE_ForEachModFile(const char* relative,
                         void (*fn)(const char* absPath, const char* modName, void* ctx),
                         void* ctx);

// The LAST enabled mod providing `relative`, for single-value settings.
// Returns false if no mod provides it. Later mods win, matching load order.
bool SWSE_FindModFile(const char* relative, char* out, int outLen);

// How many enabled mods provide `relative` (for reporting and the self-test).
int SWSE_CountModFile(const char* relative);

// <game root>\SWSEMods - for messages that need to name the folder.
const char* SWSE_ModsRoot();
