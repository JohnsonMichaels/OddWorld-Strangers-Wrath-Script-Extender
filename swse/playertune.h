// SWSE player tuning - playerprefs.txt.
//
// The player's own numbers as a file, applied automatically on every level:
//
//     health     = 500     max AND current health (all three stored copies)
//     stamina    = 300     max AND current stamina
//     speed      = 16      run speed        (the player's own motion objects)
//     jump       = 6       jump height      (the player's own motion objects)
//     gravity    = 27      fall gravity     (GlobalMotionPrefs - EVERYONE)
//     aircontrol = 1.5     midair steering  (GlobalMotionPrefs - EVERYONE)
//
// A key left blank (`health =`) or absent keeps the game's own value. This is
// exactly the file Stranger: Armed to the Teeth writes for its Player Health
// and Player Stamina fields; speed/jump/gravity/aircontrol are SWSE additions
// that the same file can carry.
//
// WHEN. Once per level, a few seconds after the level watcher sees the player
// appear - after the game's own difficulty-based initialisation, rather than
// racing it (the game sets 600/300/150 health for easy/normal/hard). If the game
// later puts max health or stamina back to its own number (a checkpoint reload
// re-initialising the player, say), the value is applied again. A value changed
// by anything else - the `hp` command, an artifact - is left alone.
//
// Every value is captured before the first write, so `playertune restore` and
// switching the feature off put the game's own numbers back.
#pragma once

// (Re)read playerprefs.txt. Returns how many keys carry a value.
int  SWSE_PlayerTuneLoad(char* msg, int msgLen);

// Per frame, from the frame hook. Cheap unless something is due.
void SWSE_PlayerTuneTick();

// Apply now (if a level is up). Returns how many values were written.
int  SWSE_PlayerTuneApply(char* msg, int msgLen);

// Put back every value captured before this level's first apply.
int  SWSE_PlayerTuneRestore(char* msg, int msgLen);

// Human-readable state, one line per call of `emit`.
void SWSE_PlayerTuneStatus(void (*emit)(const char*));

// For the self-test: values configured, and 1 if applied in this level.
void SWSE_PlayerTuneStats(int* values, int* appliedHere);
