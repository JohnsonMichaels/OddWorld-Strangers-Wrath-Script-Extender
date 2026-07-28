// SWSE feature switches - the Mod Loader's on/off panel.
//
// SWSE is several independent systems living in one DLL: the graphics
// pipeline, the console, additive hit reactions, foliage wind, AI tuning, HD
// texture replacement. A player who wants HD textures should not have to take
// the graphics pipeline with them, and a modder debugging one system wants the
// others out of the way.
//
// Read once at startup from SWSEMods\features.txt. Everything defaults to ON
// when the file is absent, so an existing install keeps behaving exactly as it
// did before this file existed.
//
// A disabled feature is not merely inert - its hooks are never installed. That
// matters: "off" should mean the game runs as if SWSE never touched it, not
// that a hook runs and returns early.
#pragma once

enum SwseFeature {
    FEAT_CONSOLE = 0,   // in-game console overlay + remote mailbox
    FEAT_GRAPHICS,      // post-process pipeline (RTGI etc.)
    FEAT_HDTEXTURES,    // .oft texture replacement at upload
    FEAT_HITREACT,      // additive hit reactions
    FEAT_FOLIAGE,       // foliage identification + wind
    FEAT_AITUNING,      // aiprefs.txt NPC tuning
    FEAT_COUNT
};

// Load SWSEMods\features.txt. Safe to call more than once; only the first
// call reads the file.
void SWSE_FeaturesInit();

// Is a feature enabled? Defaults to true before Init runs, so nothing can be
// silently skipped by an ordering mistake.
bool SWSE_Feature(SwseFeature f);

// Human-readable name, for the console and the self-test.
const char* SWSE_FeatureName(SwseFeature f);

// True if features.txt existed (so the console can say "defaults" honestly).
bool SWSE_FeaturesFromFile();
