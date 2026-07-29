// SWSE triggers - new game events, as data.
//
// The gap this fills: everything else SWSE exposes is a setting a system reads.
// A trigger is the first thing that lets a mod REACT - "when the player walks
// back into the canyon, there is a 40% chance three heavies are waiting". That
// needs no C++, only a text file in a mod folder.
//
// WHAT FIRES A TRIGGER
//   enter <label|x y z> [radius R]   player comes within R of a point
//   leave <label|x y z> [radius R]   player leaves that radius
//   levelload                        a level finished loading
//   every <seconds>                  a repeating timer
//   killed <typehash> [count N]      N NPCs of a type have died since load
//   cleared <typehash>               no live NPCs of that type remain
//
// WHAT IT DOES
//   do = <any console command>
// Actions are console commands, so a trigger inherits the entire command set
// and every command added later. That is the whole reason this is small.
//
// CONTROLLING HOW OFTEN
//   chance   = 40        percent, rolled when the condition first becomes true
//   cooldown = 120       seconds before it can fire again
//   once     = level     at most once per level load  (or `once = ever`)
//   level    = lm_level_03   only in that level
//
// EXAMPLE - the barren-after-combat problem
//   [canyon_ambush]
//   when     = enter enemyambush1 radius 18
//   level    = lm_level_03
//   chance   = 40
//   cooldown = 300
//   do       = spawnat enemyambush1 3 F4DC66D8
//   do       = combatmusic 1
//
// HONEST LIMIT: the engine never creates NPCs at runtime, so a "spawn" moves
// NPCs that already exist in the level. If its cast is dead there is nobody
// left to move and the trigger fires but places nothing - by design, rather
// than pretending.
#pragma once

// Load triggers.txt from every enabled mod. Returns the number parsed.
int SWSE_TriggersLoad();

// Per-frame evaluation. Cheap: a few float compares per trigger, no scanning.
void SWSE_TriggersTick();

// Report state for the console.
int         SWSE_TriggerCount();
const char* SWSE_TriggerName(int i);
int         SWSE_TriggerFireCount(int i);
const char* SWSE_TriggerWhen(int i);
bool        SWSE_TriggerArmed(int i);

// Fire one by name, ignoring chance/cooldown, for testing.
int SWSE_TriggerTest(const char* name, char* msg, int msgLen);

// Enable/disable the whole system at runtime.
void SWSE_TriggersEnable(int on);
int  SWSE_TriggersEnabled();
