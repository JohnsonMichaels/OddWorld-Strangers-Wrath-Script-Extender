// SWSE named positions - see positions.h.

#include "positions.h"
#include "modregistry.h"
#include "scriptvm.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#define MAX_POS 256

struct NamedPos {
    char  label[48];
    float x, y, z;
    float yaw;
    bool  hasYaw;
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
static void AddPos(const char* label, float x, float y, float z, const char* lvl,
                   bool hasYaw, float yaw) {
    NamedPos* p = nullptr;
    for (int i = 0; i < g_posN; i++)
        if (!lstrcmpiA(g_pos[i].label, label)) { p = &g_pos[i]; break; }
    if (!p) {
        if (g_posN >= MAX_POS) return;
        p = &g_pos[g_posN++];
        lstrcpynA(p->label, label, sizeof(p->label));
    }
    p->x = x; p->y = y; p->z = z;
    p->hasYaw = hasYaw;
    p->yaw = hasYaw ? yaw : 0.0f;
    lstrcpynA(p->level, lvl ? lvl : "", sizeof(p->level));
}

// A token that is entirely a number.
static bool IsNumber(const char* s, float* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    double d = strtod(s, &end);
    if (end == s || *end) return false;
    // strtod takes "nan" and "inf"; neither is a place or a facing, and a
    // NaN facing handed to SetFacing leaves the player with a NaN matrix.
    if (!_finite(d) || d > 1.0e7 || d < -1.0e7) return false;
    *out = (float)d;
    return true;
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

        char label[48] = {0}, a[40] = {0}, b[40] = {0};
        float x = 0, y = 0, z = 0;
        // label x y z [level|yaw] [yaw|level] - a number is the yaw, a word is
        // the level, in either order (positions.txt vs sites.txt order).
        int n = sscanf_s(s, "%47s %f %f %f %39s %39s", label, (unsigned)sizeof(label),
                         &x, &y, &z, a, (unsigned)sizeof(a), b, (unsigned)sizeof(b));
        if (n < 4) continue;
        if (!_finite(x) || !_finite(y) || !_finite(z)) continue;
        const char* lvl = "";
        bool hasYaw = false; float yaw = 0.0f, v = 0.0f;
        if (n >= 5) { if (IsNumber(a, &v)) { hasYaw = true; yaw = v; } else lvl = a; }
        if (n >= 6) {
            if (!hasYaw && IsNumber(b, &v)) { hasYaw = true; yaw = v; }
            else if (!lvl[0] && !IsNumber(b, &v)) lvl = b;
        }
        if (!lstrcmpA(lvl, "-")) lvl = "";
        AddPos(label, x, y, z, lvl, hasYaw, yaw);
    }
    fclose(f);
    char bb[220];
    wsprintfA(bb, "positions: +%d from [%s] %s", g_posN - before, modName,
              strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
    LogP(bb);
}

int SWSE_PositionsLoad() {
    g_posN = 0;
    SWSE_ForEachModFile("positions.txt", LoadOneFile, nullptr);
    // sites.txt after, so a label written by `writepos` (both files) or by
    // another tool into sites.txt is found either way.
    SWSE_ForEachModFile("sites.txt", LoadOneFile, nullptr);
    char b[160];
    wsprintfA(b, "positions: %d label(s) from %d positions.txt + %d sites.txt",
              g_posN, SWSE_CountModFile("positions.txt"), SWSE_CountModFile("sites.txt"));
    LogP(b);
    return g_posN;
}

static const NamedPos* Find(const char* label) {
    if (!label || !*label) return nullptr;
    // Backward, so the last definition wins - same rule as everywhere else.
    for (int i = g_posN - 1; i >= 0; i--)
        if (!lstrcmpiA(g_pos[i].label, label)) return &g_pos[i];
    return nullptr;
}

bool SWSE_PositionGet(const char* label, float* xyz3, const char** level) {
    const NamedPos* p = Find(label);
    if (!p) return false;
    if (xyz3) { xyz3[0] = p->x; xyz3[1] = p->y; xyz3[2] = p->z; }
    if (level) *level = p->level;
    return true;
}

bool SWSE_PositionYaw(const char* label, float* yawDeg) {
    const NamedPos* p = Find(label);
    if (!p || !p->hasYaw) return false;
    if (yawDeg) *yawDeg = p->yaw;
    return true;
}

int         SWSE_PositionCount()          { return g_posN; }
const char* SWSE_PositionName(int i)      { return (i >= 0 && i < g_posN) ? g_pos[i].label : ""; }
bool        SWSE_PositionAt(int i, float* xyz3, const char** level) {
    if (i < 0 || i >= g_posN) return false;
    if (xyz3) { xyz3[0] = g_pos[i].x; xyz3[1] = g_pos[i].y; xyz3[2] = g_pos[i].z; }
    if (level) *level = g_pos[i].level;
    return true;
}
bool        SWSE_PositionYawAt(int i, float* yawDeg) {
    if (i < 0 || i >= g_posN || !g_pos[i].hasYaw) return false;
    if (yawDeg) *yawDeg = g_pos[i].yaw;
    return true;
}

static void ConsoleModPath(char* out, const char* file) {
    char exe[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;   // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;         // game root
    wsprintfA(out, "%s\\SWSEMods\\SWSE Console\\%s", exe, file);
}

// Where new labels are written: always the console mod's own positions.txt.
// 1.0.x appended to whichever mod last provided the file - in a stock install
// the shipped SWSE Ambushes folder - so the player's own points lived inside a
// mod that an update replaces, and not where Stranger: Armed to the Teeth
// reads them (SWSE Console\positions.txt and sites.txt). Every mod's
// positions.txt is still READ; a mod author moves lines into their own mod.
static void WritablePath(char* out, int outLen) {
    (void)outLen;
    ConsoleModPath(out, "positions.txt");
}

// Remove every line for `label` from a points file (atomically: written
// beside, then swapped in). Used when a label is re-saved without a facing:
// sites.txt loads after positions.txt, and Stranger: Armed to the Teeth reads
// sites.txt first, so an older facing line there would otherwise go on
// shadowing the newer point in both.
static void DropLabel(const char* path, const char* label) {
    FILE* in = fopen(path, "r");
    if (!in) return;
    char tmp[MAX_PATH + 8];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);
    FILE* out = fopen(tmp, "w");
    if (!out) { fclose(in); return; }
    char line[400];
    int dropped = 0;
    while (fgets(line, sizeof(line), in)) {
        char first[48] = { 0 };
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '#' && *p != ';' &&
            sscanf_s(p, "%47s", first, (unsigned)sizeof(first)) == 1 &&
            !lstrcmpiA(first, label)) { dropped++; continue; }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (dropped && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return;
    DeleteFileA(tmp);
}

int SWSE_PositionWrite(const char* labelIn, char* msg, int msgLen) {
    // Labels are stored in 48 bytes; cut one typed longer here, before it is
    // formatted anywhere (a 250-character label used to overflow `tmp`).
    char label[48];
    lstrcpynA(label, labelIn ? labelIn : "", sizeof(label));
    char tmp[1100];
    float p[3];
    // 1 only: -2 (a fault reading it) used to count as success and write
    // whatever was in p[].
    if (SWSE_PosGet(p) != 1) {
        lstrcpynA(msg, "no player position (load a save first)", msgLen);
        return 0;
    }
    if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) {
        lstrcpynA(msg, "player at origin - level still loading", msgLen);
        return 0;
    }
    float yaw = 0.0f;
    bool hasYaw = SWSE_PlayerYawGet(&yaw) == 1;
    const char* lvl = SWSE_CurrentLevel()[0] ? SWSE_CurrentLevel() : "-";

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
    // writepos with the same name simply moves the point. The yaw goes AFTER
    // the level so a 1.0.x reader, which stops at the level, is unaffected.
    if (hasYaw)
        fprintf(f, "%-24s %10.2f %10.2f %10.2f   %-14s %7.2f\n",
                label, p[0], p[1], p[2], lvl, yaw);
    else
        fprintf(f, "%-24s %10.2f %10.2f %10.2f   %s\n", label, p[0], p[1], p[2], lvl);
    fclose(f);

    // The same point in sites.txt order, which is where Stranger: Armed to the
    // Teeth looks for a placement's facing: label x y z yaw [level]. Only with
    // a real yaw - a made-up 0 there would be read as a facing.
    char sites[MAX_PATH];
    ConsoleModPath(sites, "sites.txt");
    if (hasYaw) {
        FILE* s = fopen(sites, "a");
        if (s) {
            fprintf(s, "%-24s %10.2f %10.2f %10.2f %7.2f   %s\n",
                    label, p[0], p[1], p[2], yaw, lvl);
            fclose(s);
        }
    } else {
        DropLabel(sites, label);
    }

    AddPos(label, p[0], p[1], p[2], SWSE_CurrentLevel(), hasYaw, yaw);

    if (hasYaw)
        wsprintfA(tmp, "%s '%s' at %d %d %d facing %d deg%s%s",
                  replacing ? "moved" : "saved", label,
                  (int)p[0], (int)p[1], (int)p[2], (int)(yaw + (yaw < 0 ? -0.5f : 0.5f)),
                  SWSE_CurrentLevel()[0] ? " in " : "", SWSE_CurrentLevel());
    else
        wsprintfA(tmp, "%s '%s' at %d %d %d (facing unknown)%s%s",
                  replacing ? "moved" : "saved", label,
                  (int)p[0], (int)p[1], (int)p[2],
                  SWSE_CurrentLevel()[0] ? " in " : "", SWSE_CurrentLevel());
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}
