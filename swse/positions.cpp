// SWSE named positions - see positions.h.

#include "positions.h"
#include "modregistry.h"
#include "scriptvm.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_POS 256

struct NamedPos {
    char  label[48];
    float x, y, z;
    char  level[40];
};

static NamedPos g_pos[MAX_POS];
static int      g_posN = 0;
static char     g_level[40] = "";

static void LogP(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
    char full[MAX_PATH];
    wsprintfA(full, "%s\\swse_log.txt", path);
    FILE* f = fopen(full, "a");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
}

const char* SWSE_CurrentLevel() { return g_level; }

void SWSE_NoteLevel(const char* name) {
    if (!name || !*name) return;
    // Keep only the leaf name: callers pass things like
    // "/data/bundles/region_03/lm_level_03.lvl".
    const char* leaf = name;
    for (const char* p = name; *p; p++)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    lstrcpynA(g_level, leaf, sizeof(g_level));
    char* dot = strrchr(g_level, '.');
    if (dot) *dot = 0;
}

// A later definition of the same label REPLACES the earlier one, so a mod can
// move an ambush point that an earlier mod placed.
static void AddPos(const char* label, float x, float y, float z, const char* lvl) {
    for (int i = 0; i < g_posN; i++) {
        if (lstrcmpiA(g_pos[i].label, label)) continue;
        g_pos[i].x = x; g_pos[i].y = y; g_pos[i].z = z;
        lstrcpynA(g_pos[i].level, lvl ? lvl : "", sizeof(g_pos[i].level));
        return;
    }
    if (g_posN >= MAX_POS) return;
    NamedPos* p = &g_pos[g_posN++];
    lstrcpynA(p->label, label, sizeof(p->label));
    p->x = x; p->y = y; p->z = z;
    lstrcpynA(p->level, lvl ? lvl : "", sizeof(p->level));
}

static void LoadOneFile(const char* path, const char* modName, void*) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    int before = g_posN;
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == ';' || *s == '\n' || *s == '\r') continue;

        char label[48] = {0}, lvl[40] = {0};
        float x = 0, y = 0, z = 0;
        // label x y z [level]
        int n = sscanf(s, "%47s %f %f %f %39s", label, &x, &y, &z, lvl);
        if (n < 4) continue;
        AddPos(label, x, y, z, lvl);
    }
    fclose(f);
    char b[220];
    wsprintfA(b, "positions: +%d from [%s]", g_posN - before, modName);
    LogP(b);
}

int SWSE_PositionsLoad() {
    g_posN = 0;
    SWSE_ForEachModFile("positions.txt", LoadOneFile, nullptr);
    char b[160];
    wsprintfA(b, "positions: %d label(s) from %d mod(s)",
              g_posN, SWSE_CountModFile("positions.txt"));
    LogP(b);
    return g_posN;
}

bool SWSE_PositionGet(const char* label, float* xyz3, const char** level) {
    if (!label || !*label) return false;
    // Backward, so the last definition wins - same rule as everywhere else.
    for (int i = g_posN - 1; i >= 0; i--) {
        if (lstrcmpiA(g_pos[i].label, label)) continue;
        if (xyz3) { xyz3[0] = g_pos[i].x; xyz3[1] = g_pos[i].y; xyz3[2] = g_pos[i].z; }
        if (level) *level = g_pos[i].level;
        return true;
    }
    return false;
}

int         SWSE_PositionCount()          { return g_posN; }
const char* SWSE_PositionName(int i)      { return (i >= 0 && i < g_posN) ? g_pos[i].label : ""; }
bool        SWSE_PositionAt(int i, float* xyz3, const char** level) {
    if (i < 0 || i >= g_posN) return false;
    if (xyz3) { xyz3[0] = g_pos[i].x; xyz3[1] = g_pos[i].y; xyz3[2] = g_pos[i].z; }
    if (level) *level = g_pos[i].level;
    return true;
}

// Where new labels are written. Prefers a mod that already has a positions.txt
// (so `writepos` extends the file you are actually authoring), otherwise falls
// back to the console mod, which always exists.
static void WritablePath(char* out, int outLen) {
    if (SWSE_FindModFile("positions.txt", out, outLen)) return;
    char exe[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;   // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;         // game root
    wsprintfA(out, "%s\\SWSEMods\\SWSE Console\\positions.txt", exe);
}

int SWSE_PositionWrite(const char* label, char* msg, int msgLen) {
    char tmp[300];
    float p[3];
    if (!SWSE_PosGet(p)) {
        lstrcpynA(msg, "no player position (load a save first)", msgLen);
        return 0;
    }
    if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) {
        lstrcpynA(msg, "player at origin - level still loading", msgLen);
        return 0;
    }

    char path[MAX_PATH];
    WritablePath(path, sizeof(path));

    bool replacing = SWSE_PositionGet(label, nullptr, nullptr);

    FILE* f = fopen(path, "a");
    if (!f) {
        wsprintfA(tmp, "could not write %s", path);
        lstrcpynA(msg, tmp, msgLen);
        return -1;
    }
    // Appending (rather than rewriting) keeps any comments the author wrote.
    // A repeated label is fine: the loader takes the last one, so re-running
    // writepos with the same name simply moves the point.
    fprintf(f, "%-24s %10.2f %10.2f %10.2f   %s\n",
            label, p[0], p[1], p[2],
            SWSE_CurrentLevel()[0] ? SWSE_CurrentLevel() : "-");
    fclose(f);

    AddPos(label, p[0], p[1], p[2], SWSE_CurrentLevel());

    wsprintfA(tmp, "%s '%s' at %d %d %d%s%s",
              replacing ? "moved" : "saved", label,
              (int)p[0], (int)p[1], (int)p[2],
              SWSE_CurrentLevel()[0] ? " in " : "",
              SWSE_CurrentLevel());
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}
