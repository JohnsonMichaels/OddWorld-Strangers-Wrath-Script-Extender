// SWSE feature switches - see features.h.

#include "features.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

static bool g_on[FEAT_COUNT];
static bool g_loaded = false;
static bool g_fromFile = false;

static const char* kNames[FEAT_COUNT] = {
    "console", "graphics", "hdtextures", "hitreact", "foliage", "aituning",
    "triggers"
};

const char* SWSE_FeatureName(SwseFeature f) {
    return (f >= 0 && f < FEAT_COUNT) ? kNames[f] : "?";
}

bool SWSE_FeaturesFromFile() { return g_fromFile; }

bool SWSE_Feature(SwseFeature f) {
    if (f < 0 || f >= FEAT_COUNT) return false;
    // Default ON before Init: a feature must never be skipped because of an
    // initialisation-order mistake. Failing open here is the safe direction,
    // since the previous behaviour was "everything on, always".
    if (!g_loaded) return true;
    return g_on[f];
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

void SWSE_FeaturesInit() {
    if (g_loaded) return;
    for (int i = 0; i < FEAT_COUNT; i++) g_on[i] = true;   // absent file = all on

    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;   // ...\bin
    sl = strrchr(path, '\\'); if (sl) *sl = 0;         // game root
    char file[MAX_PATH];
    wsprintfA(file, "%s\\SWSEMods\\features.txt", path);

    HANDLE h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        g_loaded = true;
        LogF("features: no features.txt - all features ON");
        return;
    }
    static char buf[4096];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    g_fromFile = true;

    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#' || *line == ';' || *line == '\r') continue;

        char key[32] = {0}, val[32] = {0};
        int ki = 0, vi = 0; bool eq = false;
        for (char* c = line; *c; c++) {
            if (*c == '#' || *c == ';' || *c == '\r') break;
            if (*c == '=') { eq = true; continue; }
            if (*c == ' ' || *c == '\t') continue;
            if (!eq) { if (ki < 31) key[ki++] = *c; }
            else     { if (vi < 31) val[vi++] = *c; }
        }
        if (!key[0] || !val[0]) continue;
        for (int i = 0; i < FEAT_COUNT; i++) {
            if (lstrcmpiA(key, kNames[i])) continue;
            g_on[i] = Truthy(val);
            break;
        }
    }
    g_loaded = true;

    char b[240] = "features:";
    for (int i = 0; i < FEAT_COUNT; i++) {
        char one[48];
        wsprintfA(one, " %s=%s", kNames[i], g_on[i] ? "on" : "OFF");
        lstrcatA(b, one);
    }
    LogF(b);
}
