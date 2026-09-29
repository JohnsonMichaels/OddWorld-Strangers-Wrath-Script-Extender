// SWSE feature switches - see features.h.

#include "features.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Built-ins first, then plugin switches (1.1) in the order plugins.cpp added
// them. Every per-switch array is sized for both.
#define FEAT_MAX (FEAT_COUNT + SWSE_MAX_PLUGIN_FEATS)

static bool g_on[FEAT_MAX];
static bool g_saved[FEAT_MAX];        // what features.txt holds (temp switches excluded)
static bool g_auto[FEAT_MAX];         // ...and whether that is `auto` (see features.h)
static bool g_loaded = false;
static bool g_fromFile = false;

static const char* kNames[FEAT_COUNT] = {
    "console", "graphics", "hdtextures", "hitreact", "foliage", "aituning",
    "triggers", "npctuning", "playertune", "prefsedit", "raytrace"
};

static const char* kDesc[FEAT_COUNT] = {
    "in-game console (~) and the remote command mailbox",
    "post-process pipeline: AO, RTGI, bloom, grade (F10 toggles the look)",
    "HD texture replacement (.oft files swapped in at upload)",
    "additive hit reactions - NPCs flinch where they are shot",
    "foliage wind: grass and plants sway and part around you",
    "aiprefs.txt enemy tuning: sight, fire rate, reload, accuracy, miss time",
    "triggers.txt mod events (ambushes and other reactions)",
    "characters.txt / console.txt per-character health and gib, every level",
    "playerprefs.txt: player health, stamina, speed, jump, every level",
    "prefs.txt: live edits to any loaded prefs record, every level",
    "ray-traced ambient occlusion - EXPERIMENTAL; needs graphics on",
};

static const struct { const char* alias; int feat; } kAlias[] = {
    { "grass", FEAT_FOLIAGE },      { "wind", FEAT_FOLIAGE },
    { "plants", FEAT_FOLIAGE },     { "hd", FEAT_HDTEXTURES },
    { "textures", FEAT_HDTEXTURES },{ "gfx", FEAT_GRAPHICS },
    { "postfx", FEAT_GRAPHICS },    { "rtgi", FEAT_GRAPHICS },
    { "ai", FEAT_AITUNING },        { "difficulty", FEAT_AITUNING },
    { "ambushes", FEAT_TRIGGERS },  { "characters", FEAT_NPCTUNING },
    { "tuning", FEAT_NPCTUNING },   { "player", FEAT_PLAYERTUNE },
    { "playerprefs", FEAT_PLAYERTUNE }, { "prefs", FEAT_PREFSEDIT },
    { "rt", FEAT_RAYTRACE },        { "rtao", FEAT_RAYTRACE },
    { "raytracing", FEAT_RAYTRACE },
    { "combat", FEAT_HITREACT },    { "hitreactions", FEAT_HITREACT },
};
static const int N_ALIAS = (int)(sizeof(kAlias) / sizeof(kAlias[0]));

// ---- plugin switches (1.1) ---------------------------------------------------
// The name is the DLL's file name, fixed at discovery. The description and
// nicknames arrive once the plugin has been loaded and has described itself.
#define PF_ALIASES 8
struct PluginFeat {
    char name[32];
    char desc[128];
    char alias[PF_ALIASES][32];
    int  aliasN;
};
static PluginFeat g_pf[SWSE_MAX_PLUGIN_FEATS];
static int        g_pfN = 0;

// features.txt lines naming no built-in switch, kept from the first read: a
// plugin's switch is added later (at discovery), and its value must be the
// one the file held. Unknown keys were always ignored on read and kept on save.
#define MAX_FILE_KEYS 128
struct FileKey { char key[32]; bool on; };
static FileKey g_fileKey[MAX_FILE_KEYS];
static int     g_fileKeyN = 0;

static bool IsPluginId(int f) { return f >= FEAT_COUNT && f < FEAT_COUNT + g_pfN; }
static bool ValidId(int f)    { return f >= 0 && f < FEAT_COUNT + g_pfN; }

int  SWSE_FeatureTotal()         { return FEAT_COUNT + g_pfN; }
bool SWSE_FeatureIsPlugin(int f) { return IsPluginId(f); }

// Only the console is on unless the user asks for more; plugins are off.
static bool DefaultOf(int f) { return f == FEAT_CONSOLE; }
// The two systems whose own files already say whether to act. Never a
// plugin: a DLL runs only when the user names it.
static bool AutoCapable(int f) { return f == FEAT_AITUNING || f == FEAT_PLAYERTUNE; }

bool SWSE_FeatureDefault(SwseFeature f) {
    return ValidId(f) ? DefaultOf(f) : false;
}

const char* SWSE_FeatureName(SwseFeature f) {
    if (f >= 0 && f < FEAT_COUNT) return kNames[f];
    if (IsPluginId(f)) return g_pf[f - FEAT_COUNT].name;
    return "?";
}

const char* SWSE_FeatureDescribe(SwseFeature f) {
    if (f >= 0 && f < FEAT_COUNT) return kDesc[f];
    if (IsPluginId(f)) return g_pf[f - FEAT_COUNT].desc;
    return "";
}

bool SWSE_FeaturesFromFile() { return g_fromFile; }

bool SWSE_Feature(SwseFeature f) {
    if (!ValidId(f)) return false;
    // Before Init: the default, not "on". 1.0.x failed open here because
    // everything defaulted on; with a console-only default, failing open would
    // switch on exactly the systems the user never asked for.
    if (!g_loaded) return DefaultOf(f);
    return g_on[f];
}

void SWSE_FeatureSetFlag(SwseFeature f, bool on) {
    if (!g_loaded) SWSE_FeaturesInit();
    if (!ValidId(f)) return;
    g_on[f] = on;
}

void SWSE_FeatureSetSaved(SwseFeature f, bool on) {
    if (!g_loaded) SWSE_FeaturesInit();
    if (!ValidId(f)) return;
    g_saved[f] = on;
    g_auto[f] = false;                    // an explicit on/off replaces auto
}

bool SWSE_FeatureAutoCapable(SwseFeature f) { return f >= 0 && f < FEAT_COUNT && AutoCapable(f); }

bool SWSE_FeatureIsAuto(SwseFeature f) {
    if (!g_loaded) SWSE_FeaturesInit();
    if (!ValidId(f)) return false;
    return g_auto[f];
}

void SWSE_FeatureSetSavedAuto(SwseFeature f) {
    if (f < 0 || f >= FEAT_COUNT || !AutoCapable(f)) return;
    if (!g_loaded) SWSE_FeaturesInit();
    g_auto[f] = true;
    g_saved[f] = false;
}

bool SWSE_FeatureSaved(SwseFeature f) {
    if (!g_loaded) SWSE_FeaturesInit();
    if (!ValidId(f)) return false;
    return g_saved[f];
}

bool SWSE_FeatureNameReserved(const char* name) {
    if (!name || !*name) return false;
    // The `features` command's own words: `features all on`, `features preset`.
    if (!lstrcmpiA(name, "all") || !lstrcmpiA(name, "preset")) return true;
    for (int i = 0; i < FEAT_COUNT; i++) if (!lstrcmpiA(name, kNames[i])) return true;
    for (int i = 0; i < N_ALIAS; i++) if (!lstrcmpiA(name, kAlias[i].alias)) return true;
    return false;
}

// Canonical names before nicknames, built-ins before plugins. A plugin can
// never hold a built-in's name or nickname, and a plugin nickname that
// collides with anything is dropped when it is registered, so the order only
// has to be stated, not relied on.
int SWSE_FeatureFind(const char* name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < FEAT_COUNT; i++)
        if (!lstrcmpiA(name, kNames[i])) return i;
    for (int i = 0; i < g_pfN; i++)
        if (!lstrcmpiA(name, g_pf[i].name)) return FEAT_COUNT + i;
    for (int i = 0; i < N_ALIAS; i++)
        if (!lstrcmpiA(name, kAlias[i].alias)) return kAlias[i].feat;
    for (int i = 0; i < g_pfN; i++)
        for (int k = 0; k < g_pf[i].aliasN; k++)
            if (!lstrcmpiA(name, g_pf[i].alias[k])) return FEAT_COUNT + i;
    return -1;
}

static void LogF(const char* s) {
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

static bool Truthy(const char* v) {
    return !lstrcmpiA(v, "on") || !lstrcmpiA(v, "true") ||
           !lstrcmpiA(v, "yes") || !lstrcmpiA(v, "1") || !lstrcmpiA(v, "enabled");
}

static void FeaturesPath(char* out) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;   // ...\bin
    sl = strrchr(path, '\\'); if (sl) *sl = 0;         // game root
    wsprintfA(out, "%s\\SWSEMods\\features.txt", path);
}

// The whole file, NUL-terminated, in a heap buffer the caller frees - or
// nullptr. 1.0.x read the first 8 KB, and the save then wrote the cut-down
// copy back over the original.
static char* ReadWhole(const char* file, DWORD* gotOut) {
    *gotOut = 0;
    HANDLE h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) { CloseHandle(h); return nullptr; }
    char* buf = (char*)malloc(size + 1);
    if (!buf) { CloseHandle(h); return nullptr; }
    DWORD got = 0;
    if (!ReadFile(h, buf, size, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    *gotOut = got;
    return buf;
}

// Split "key = value  # comment" into key and value. Returns false for
// comments, blanks and anything without both halves.
static bool ParseLine(const char* line, char* key, char* val) {
    key[0] = val[0] = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || *line == ';' || *line == '\r' || *line == '\n')
        return false;
    int ki = 0, vi = 0; bool eq = false;
    for (const char* c = line; *c; c++) {
        if (*c == '#' || *c == ';' || *c == '\r' || *c == '\n') break;
        if (*c == '=') { eq = true; continue; }
        if (*c == ' ' || *c == '\t') continue;
        if (!eq) { if (ki < 31) key[ki++] = *c; }
        else     { if (vi < 31) val[vi++] = *c; }
    }
    key[ki] = 0; val[vi] = 0;
    return key[0] && val[0];
}

static void RememberFileKey(const char* key, bool on) {
    for (int i = 0; i < g_fileKeyN; i++)
        if (!lstrcmpiA(g_fileKey[i].key, key)) { g_fileKey[i].on = on; return; }   // later line wins
    if (g_fileKeyN >= MAX_FILE_KEYS) return;
    lstrcpynA(g_fileKey[g_fileKeyN].key, key, 32);
    g_fileKey[g_fileKeyN].on = on;
    g_fileKeyN++;
}

void SWSE_FeaturesInit() {
    if (g_loaded) return;
    // Not mentioned = the default: off, except the console - and `auto` for
    // the two auto-capable systems. Auto starts nothing here; the first
    // frame asks each system's own file (framehook.cpp).
    for (int i = 0; i < FEAT_COUNT; i++) { g_on[i] = g_saved[i] = DefaultOf(i); g_auto[i] = AutoCapable(i); }

    char file[MAX_PATH];
    FeaturesPath(file);

    DWORD got = 0;
    char* buf = ReadWhole(file, &got);
    if (!buf) {
        g_loaded = true;
        LogF("features: no features.txt - console only (the 1.1 default)");
        return;
    }
    g_fromFile = true;

    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char key[32], val[32];
        if (!ParseLine(line, key, val)) continue;
        bool known = false;
        for (int i = 0; i < FEAT_COUNT; i++) {
            if (lstrcmpiA(key, kNames[i])) continue;
            if (!lstrcmpiA(val, "auto") && AutoCapable(i)) {
                g_on[i] = g_saved[i] = false;
                g_auto[i] = true;
            } else {
                g_on[i] = g_saved[i] = Truthy(val);
                g_auto[i] = false;
            }
            known = true;
            break;
        }
        // Not a built-in: possibly a plugin's switch, which does not exist
        // yet (plugins are discovered on the first frame).
        if (!known) RememberFileKey(key, Truthy(val));
    }
    free(buf);
    g_loaded = true;

    char b[320] = "features:";
    for (int i = 0; i < FEAT_COUNT; i++) {
        char one[48];
        wsprintfA(one, " %s=%s", kNames[i], g_auto[i] ? "auto" : g_on[i] ? "on" : "off");
        lstrcatA(b, one);
    }
    LogF(b);
}

// ---- plugin switches -----------------------------------------------------------
static bool ValidKeyName(const char* n) {
    int len = n ? lstrlenA(n) : 0;
    if (len < 1 || len > 31) return false;
    for (int i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

int SWSE_FeatureAddPlugin(const char* name) {
    if (!g_loaded) SWSE_FeaturesInit();
    if (!ValidKeyName(name) || SWSE_FeatureNameReserved(name)) return -1;
    for (int i = 0; i < g_pfN; i++)
        if (!lstrcmpiA(g_pf[i].name, name)) return FEAT_COUNT + i;     // a rescan
    if (g_pfN >= SWSE_MAX_PLUGIN_FEATS) return -1;
    PluginFeat& pf = g_pf[g_pfN];
    memset(&pf, 0, sizeof(pf));
    lstrcpynA(pf.name, name, 32);
    int id = FEAT_COUNT + g_pfN;
    bool on = false;
    for (int i = 0; i < g_fileKeyN; i++)
        if (!lstrcmpiA(g_fileKey[i].key, name)) { on = g_fileKey[i].on; break; }
    g_on[id] = g_saved[id] = on;
    g_auto[id] = false;
    g_pfN++;
    return id;
}

void SWSE_FeatureSetPluginInfo(int id, const char* desc, const char* aliases) {
    if (!IsPluginId(id)) return;
    PluginFeat& pf = g_pf[id - FEAT_COUNT];
    if (desc) lstrcpynA(pf.desc, desc, sizeof(pf.desc));
    if (!aliases) return;
    // "wind, grass,plants": split on commas and spaces. Nicknames only ever
    // add up (a rescan or a second call re-offers the same ones).
    const char* s = aliases;
    while (*s) {
        while (*s == ',' || *s == ' ' || *s == '\t') s++;
        if (!*s) break;
        char a[40]; int n = 0;
        while (*s && *s != ',' && *s != ' ' && *s != '\t') { if (n < 39) a[n++] = *s; s++; }
        a[n] = 0;
        CharLowerA(a);
        bool mine = false;
        for (int k = 0; k < pf.aliasN; k++) if (!lstrcmpiA(pf.alias[k], a)) mine = true;
        if (mine) continue;
        char why[160] = "";
        if (!ValidKeyName(a))
            _snprintf_s(why, sizeof(why), _TRUNCATE, "is not a valid switch name");
        else if (SWSE_FeatureNameReserved(a))
            _snprintf_s(why, sizeof(why), _TRUNCATE, "belongs to a built-in switch");
        else {
            int other = SWSE_FeatureFind(a);
            if (other >= 0 && other != id)
                _snprintf_s(why, sizeof(why), _TRUNCATE, "already means '%s'",
                            SWSE_FeatureName((SwseFeature)other));
        }
        if (!why[0] && pf.aliasN >= PF_ALIASES)
            _snprintf_s(why, sizeof(why), _TRUNCATE, "is past the first %d nicknames", PF_ALIASES);
        if (why[0]) {
            char b[300];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "features: plugin '%s' nickname '%s' dropped - it %s",
                        pf.name, a, why);
            LogF(b);
            continue;
        }
        lstrcpynA(pf.alias[pf.aliasN++], a, 32);
    }
}

void SWSE_FeaturesForEachUnclaimed(void (*fn)(const char*, bool, void*), void* ctx) {
    if (!fn) return;
    if (!g_loaded) SWSE_FeaturesInit();
    for (int i = 0; i < g_fileKeyN; i++) {
        bool claimed = false;
        for (int k = 0; k < g_pfN; k++)
            if (!lstrcmpiA(g_pf[k].name, g_fileKey[i].key)) { claimed = true; break; }
        if (!claimed) fn(g_fileKey[i].key, g_fileKey[i].on, ctx);
    }
}

// A key line with only its value replaced: indentation, spacing and a
// trailing comment stay as the author wrote them. 1.0.x rewrote the whole
// line, so `foliage = on   # for the meadow levels` lost its note.
static int RewriteValue(char* dst, const char* line, const char* value) {
    const char* eq = strchr(line, '=');
    if (!eq) { int n = lstrlenA(line); memcpy(dst, line, n + 1); return n; }
    const char* vs = eq + 1;
    while (*vs == ' ' || *vs == '\t') vs++;
    const char* ve = vs;
    while (*ve && *ve != ' ' && *ve != '\t' && *ve != '#' && *ve != ';' && *ve != '\r') ve++;
    int n = (int)(vs - line);
    memcpy(dst, line, n);
    int vl = lstrlenA(value);
    memcpy(dst + n, value, vl); n += vl;
    int rest = lstrlenA(ve);
    memcpy(dst + n, ve, rest); n += rest;
    dst[n] = 0;
    return n;
}

bool SWSE_FeaturesSave(char* msg, int msgLen) {
    if (!g_loaded) SWSE_FeaturesInit();
    char file[MAX_PATH];
    FeaturesPath(file);
    const int total = SWSE_FeatureTotal();

    // Read whatever is there now, so comments and hand edits survive.
    DWORD got = 0;
    char* in = ReadWhole(file, &got);
    if (!in) { in = (char*)malloc(1); if (!in) return false; in[0] = 0; got = 0; }

    // Every line can at most gain a '\r' and a value two characters longer,
    // and each appended block for a missing key is well under 320 bytes.
    int cap = (int)got * 2 + 4096 + g_pfN * 320;
    char* out = (char*)malloc(cap);
    if (!out) { free(in); lstrcpynA(msg, "out of memory", msgLen); return false; }
    int used = 0;
    bool written[FEAT_MAX] = { false };
    if (!got) {
        used += wsprintfA(out + used,
            "# SWSE - feature switches (written by the `features` command)\r\n"
            "# Only the console is on unless a line below says otherwise.\r\n\r\n");
    }

    char* p = in;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        bool hadNl = (*p == '\n');
        if (*p) *p++ = 0;
        int len = lstrlenA(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;

        char key[32], val[32];
        int feat = -1;
        if (ParseLine(line, key, val)) {
            // Plugin keys count as known: rewritten in place like built-ins.
            for (int i = 0; i < total; i++)
                if (!lstrcmpiA(key, SWSE_FeatureName((SwseFeature)i))) { feat = i; break; }
        }
        if (feat >= 0) {
            if (written[feat]) continue;          // drop duplicates of a key
            used += RewriteValue(out + used, line, g_auto[feat] ? "auto" : g_saved[feat] ? "on" : "off");
            written[feat] = true;
        } else {
            memcpy(out + used, line, len); used += len;
        }
        if (hadNl || *p) { out[used++] = '\r'; out[used++] = '\n'; }
    }
    free(in);
    for (int i = 0; i < total; i++) {
        if (written[i]) continue;
        const char* d = SWSE_FeatureDescribe((SwseFeature)i);
        char desc[160];
        if (!IsPluginId(i))  _snprintf_s(desc, sizeof(desc), _TRUNCATE, "%s", d);
        else if (d && *d)    _snprintf_s(desc, sizeof(desc), _TRUNCATE, "plugin: %s", d);
        else                 _snprintf_s(desc, sizeof(desc), _TRUNCATE, "plugin %s",
                                         SWSE_FeatureName((SwseFeature)i));
        int n = _snprintf_s(out + used, cap - used, _TRUNCATE, "\r\n# %s\r\n%-10s = %s\r\n",
                            desc, SWSE_FeatureName((SwseFeature)i),
                            g_auto[i] ? "auto" : g_saved[i] ? "on" : "off");
        if (n > 0) used += n;
    }

    // Write beside, then swap in, so a failure never leaves a half file.
    char tmp[MAX_PATH];
    wsprintfA(tmp, "%s.tmp", file);
    HANDLE w = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (w == INVALID_HANDLE_VALUE) {
        free(out);
        char t[300]; wsprintfA(t, "could not write %s", tmp);
        lstrcpynA(msg, t, msgLen);
        return false;
    }
    DWORD wrote = 0;
    WriteFile(w, out, used, &wrote, NULL);
    CloseHandle(w);
    free(out);
    if (!MoveFileExA(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        char t[300]; wsprintfA(t, "could not replace %s", file);
        lstrcpynA(msg, t, msgLen);
        return false;
    }
    g_fromFile = true;
    lstrcpynA(msg, "saved to SWSEMods\\features.txt", msgLen);
    return true;
}
