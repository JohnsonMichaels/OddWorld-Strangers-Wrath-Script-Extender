// SWSE game-build identity, and the safe mode an unknown build runs in.
//
// Every address SWSE uses inside stranger.exe - hook targets, vtables, game
// functions, globals such as the ResourceManager - was measured on ONE build:
// the Steam release of Oddworld: Stranger's Wrath HD. Another build (GOG, a
// future patch) can put different code or data at those addresses. A hook
// written there corrupts it (the reported "console crashes on the GOG
// version"), and a call or a read through one acts on whatever is there.
//
// This identifies the running exe from its PE header (link timestamp, image
// size, entry point - none of which ASLR relocates) and answers one question:
// were SWSE's addresses measured on this build? On an unknown build SWSE runs
// in SAFE MODE: nothing that patches game code, calls a game function or reads
// the game through a Steam-measured address runs. What stays is SWSE's own:
// the console and its mailbox, the commands that only use SWSE's own state
// (`query`, `status`, `features`, `help`, `mods`, binds, aliases...), and the
// OpenGL-side systems (graphics, HD textures, foliage, ray tracing). The level
// watcher does not start, the switches that need it or a game hook refuse,
// and the console refuses every command that reaches the game (console.cpp,
// SafeModeBlocks) with SWSE_GameBuildRefusal's text.
//
// SWSE_UNKNOWN_BUILD_OK=1 in the game's environment overrides all of it, for
// testing a build once someone has checked its addresses match.
// SWSE_PRETEND_UNKNOWN_BUILD=1 does the opposite, for testing safe mode on the
// Steam build: its exe is treated as unknown (`query version` answers
// build=unknown). The override still wins over it.
#pragma once

// 1 if SWSE's measured addresses belong to this exe (or the override is set).
int SWSE_GameBuildKnown();

// 1 in safe mode: the build is unknown and nothing overrides that. The same
// as !SWSE_GameBuildKnown(), named for the places that decide what runs.
int SWSE_GameBuildSafeMode();

// A short name: "Steam HD" or "unknown".
const char* SWSE_GameBuildName();

// One line for logs and `status`: name plus the fingerprint values.
void SWSE_GameBuildDescribe(char* out, int outLen);

// Standard refusal text for anything safe mode stops, e.g. "npcspy" or "hp".
void SWSE_GameBuildRefusal(const char* what, char* out, int outLen);
