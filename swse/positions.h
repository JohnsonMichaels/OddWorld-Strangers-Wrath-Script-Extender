// SWSE named positions - "here, call it enemyambush1".
//
// A trigger needs somewhere to happen and an ambush needs somewhere to appear,
// and neither is expressible as a number a person can invent. The workable
// loop is: stand where you want it, name it, then refer to it by name forever
// after. Everything downstream (triggers, spawns, teleports) takes a label.
//
// Positions live in `positions.txt` inside a mod folder, so a mod ships its
// own ambush points and nobody edits a shared file. They are read ADDITIVELY
// from every enabled mod; a later mod redefining a label wins, matching load
// order.
//
// FILE FORMAT - one per line, '#' comments:
//     label   x y z   [level]   [yaw]
//     enemyambush1   1250.5 40.2 -300.0   lm_level_03   270.0
//
// The level name is written automatically and used as a hint: a position from
// another level is meaningless, and firing an ambush at coordinates that
// belong to a different map is the obvious way for this to go wrong.
//
// YAW (1.1). The direction you were facing, in degrees about the up (Z) axis,
// so `goto` puts you back facing the same way and a tool placing something at
// the spot can orient it. The parser takes the columns after z in either order
// - a number is the yaw, a word is the level - so 1.0.x files still load, and
// 1.0.x reads the new files (it only looks at the level column).
//
// sites.txt is the same list written in the column order Stranger: Armed to
// the Teeth reads for placements - `label x y z yaw [level]` - in the SWSE
// Console folder. `writepos` appends to both; both are loaded.
#pragma once

// Load every enabled mod's positions.txt and sites.txt. Returns how many
// labels are known.
int SWSE_PositionsLoad();

// Look up a label. Returns false if unknown. `level` may be null.
bool SWSE_PositionGet(const char* label, float* xyz3, const char** level);

// The saved yaw for a label, in degrees. False if unknown or none recorded.
bool SWSE_PositionYaw(const char* label, float* yawDeg);

// Capture the player's current position (and yaw, when it can be read) under
// `label` and append it to the writable positions file and to sites.txt.
// Returns 1 ok, 0 no player position, -1 write failed.
int SWSE_PositionWrite(const char* label, char* msg, int msgLen);

// Enumerate for listing. The yaw getters return false when a label has none.
int         SWSE_PositionCount();
const char* SWSE_PositionName(int i);
bool        SWSE_PositionAt(int i, float* xyz3, const char** level);
bool        SWSE_PositionYawAt(int i, float* yawDeg);

// The level the game is in right now, as far as SWSE can tell ("" if unknown).
// Used to warn when a label belongs somewhere else.
const char* SWSE_CurrentLevel();
void        SWSE_NoteLevel(const char* name);
