// SWSE foliage identification.
//
// The wind effect must move plants and nothing else, so the renderer has to
// know which draws are foliage. SWSE only ever sees pixel data, never names -
// but glspy already fingerprints every texture at upload, and
// tools/texmap.py computes that same fingerprint from the archives where the
// artists' names still exist. So a list of foliage fingerprints is generated
// offline (bamboo_leaves_AT, cedar_fir_tree_01_AT, ...) and matched at upload.
//
// Texture ids are tracked rather than hashes because ids are what a draw call
// sees. GL recycles ids across level loads, so the foliage flag is REWRITTEN on
// every level-0 upload - set for foliage, cleared otherwise. That is the same
// hazard that once left black textures all over the world when a stale id list
// was kept (see glspy.cpp), and it is handled the same way: never trust an id
// past the upload that defined it.
#pragma once

// Load the fingerprint list. Safe to call every frame; only acts once.
void SWSE_FoliageInit();

// Re-read foliage.txt at runtime and rebuild the flags for textures already
// uploaded. Returns the number of fingerprints loaded. Lets entries be added
// to the list and tested without restarting the game.
int SWSE_FoliageReload();

// Called by glspy for every level-0 texture upload, with the vanilla
// fingerprint and the texture id it was uploaded into.
void SWSE_FoliageNoteUpload(unsigned hash, unsigned texid);

// Per-frame counter reset, from the frame hook.
void SWSE_FoliageFrameMark();

// Install/remove the glBindTexture hook that tracks which texture is current.
// Returns 0 on failure and writes why into msg.
int SWSE_FoliageTrack(int on, char* msg, int msgLen);

// listN            fingerprints loaded from disk
// knownTexids      texture ids currently flagged as foliage
// bindsLastFrame   foliage binds counted in the previous frame
// peakBinds        highest per-frame count seen since tracking started
// hooked           1 if the bind hook is installed
void SWSE_FoliageStats(int* listN, int* knownTexids, int* bindsLastFrame,
                       int* peakBinds, int* hooked);

// True while the currently bound 2D texture is a foliage texture. This is what
// gates the wind parameter per draw.
int SWSE_FoliageCurrentIsFoliage();

// Capture every distinct texture bound during the next frame, and write their
// fingerprints to `path`. tools/texmap.py turns those back into artist names,
// which is the only reliable way to learn what a scene is actually made of -
// guessing plant names from a word list missed an entire desert biome.
void SWSE_FoliageScanRequest(const char* path);
int  SWSE_FoliageScanDone();     // 0 pending, >0 textures written, -1 failed

// The ARB vertex programs seen drawing foliage, discovered by a scan. These
// are the programs the wind displacement is injected into.
int SWSE_FoliagePrograms(unsigned* out, int maxOut);
