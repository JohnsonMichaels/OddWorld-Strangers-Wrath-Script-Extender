// SWSE mod registry - see modregistry.h.

#include "modregistry.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_MODS 64

struct ModEntry {
    char name[96];
    char path[MAX_PATH];
    bool enabled;
};

static ModEntry g_mods[MAX_MODS];
static int      g_modN = 0;
static bool     g_loaded = false;
static char     g_root[MAX_PATH] = "";

static void LogM(const char* s) {
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

const char* SWSE_ModsRoot() { return g_root; }

static void ComputeRoot() {
    char exe[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;   // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;         // game root
    wsprintfA(g_root, "%s\\SWSEMods", exe);
}

// ---- load_order.txt -------------------------------------------------------
// Line syntax: "Name" enabled, "!Name" disabled, "#..." comment.
#define MAX_ORDER 64
static char g_order[MAX_ORDER][96];
static char g_disabled[MAX_ORDER][96];
static int  g_orderN = 0, g_disabledN = 0;

static void ReadLoadOrder() {
    g_orderN = g_disabledN = 0;
    char path[MAX_PATH];
    wsprintfA(path, "%s\\load_order.txt", g_root);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    static char buf[8192];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    char* p = buf;
    // Skip a UTF-8 BOM. Notepad writes one by default, and without this the
    // first line fails the '#' comment test and the header is read as a mod
    // name. That exact bug shipped once already on the Python side.
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;

    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        // strip CR and trailing space
        int n = lstrlenA(line);
        while (n > 0 && (line[n-1] == '\r' || line[n-1] == ' ' || line[n-1] == '\t'))
            line[--n] = 0;
        if (!*line || *line == '#' || *line == ';') continue;

        bool off = (*line == '!');
        if (off) { line++; while (*line == ' ') line++; }
        if (!*line) continue;
        if (off) {
            if (g_disabledN < MAX_ORDER) lstrcpynA(g_disabled[g_disabledN++], line, 96);
        }
        if (g_orderN < MAX_ORDER) lstrcpynA(g_order[g_orderN++], line, 96);
    }
}

static bool IsDisabled(const char* name) {
    for (int i = 0; i < g_disabledN; i++)
        if (!lstrcmpiA(g_disabled[i], name)) return true;
    return false;
}

static int OrderIndex(const char* name) {
    for (int i = 0; i < g_orderN; i++)
        if (!lstrcmpiA(g_order[i], name)) return i;
    return -1;      // unlisted: sorts after everything listed
}

static void AddMod(const char* name) {
    if (g_modN >= MAX_MODS) return;
    ModEntry* m = &g_mods[g_modN];
    lstrcpynA(m->name, name, 96);
    wsprintfA(m->path, "%s\\%s", g_root, name);
    m->enabled = !IsDisabled(name);
    g_modN++;
}

int SWSE_ModsReload() {
    g_modN = 0;
    ComputeRoot();
    ReadLoadOrder();

    char glob[MAX_PATH];
    wsprintfA(glob, "%s\\*", g_root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LogM("mods: SWSEMods folder not found");
        g_loaded = true;
        return 0;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;          // ".", "..", ".backups"
        AddMod(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    // Sort into load order. Listed mods first in their listed order, then
    // unlisted ones alphabetically, so a newly dropped folder is predictable.
    for (int i = 0; i < g_modN; i++) {
        for (int j = i + 1; j < g_modN; j++) {
            int oi = OrderIndex(g_mods[i].name), oj = OrderIndex(g_mods[j].name);
            bool swap;
            if (oi < 0 && oj < 0)      swap = lstrcmpiA(g_mods[i].name, g_mods[j].name) > 0;
            else if (oi < 0)           swap = true;    // unlisted after listed
            else if (oj < 0)           swap = false;
            else                       swap = oi > oj;
            if (swap) { ModEntry t = g_mods[i]; g_mods[i] = g_mods[j]; g_mods[j] = t; }
        }
    }

    char b[400];
    int on = 0;
    for (int i = 0; i < g_modN; i++) if (g_mods[i].enabled) on++;
    wsprintfA(b, "mods: %d folder(s), %d enabled, root %s", g_modN, on, g_root);
    LogM(b);
    for (int i = 0; i < g_modN; i++) {
        wsprintfA(b, "  [%s] %s", g_mods[i].enabled ? "on " : "OFF", g_mods[i].name);
        LogM(b);
    }
    g_loaded = true;
    return on;
}

void SWSE_ModsInit() {
    if (!g_loaded) SWSE_ModsReload();
}

int SWSE_ModCount() {
    SWSE_ModsInit();
    int n = 0;
    for (int i = 0; i < g_modN; i++) if (g_mods[i].enabled) n++;
    return n;
}

static ModEntry* NthEnabled(int idx) {
    int n = 0;
    for (int i = 0; i < g_modN; i++) {
        if (!g_mods[i].enabled) continue;
        if (n == idx) return &g_mods[i];
        n++;
    }
    return nullptr;
}

const char* SWSE_ModName(int i) {
    SWSE_ModsInit();
    ModEntry* m = NthEnabled(i);
    return m ? m->name : "";
}

const char* SWSE_ModPath(int i) {
    SWSE_ModsInit();
    ModEntry* m = NthEnabled(i);
    return m ? m->path : "";
}

// Accepts a file ("foliage.txt") or a directory ("textures").
static bool ModHas(const ModEntry* m, const char* relative, char* out, int outLen) {
    char full[MAX_PATH];
    wsprintfA(full, "%s\\%s", m->path, relative);
    DWORD a = GetFileAttributesA(full);
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (out) lstrcpynA(out, full, outLen);
    return true;
}

void SWSE_ForEachModFile(const char* relative,
                         void (*fn)(const char*, const char*, void*), void* ctx) {
    SWSE_ModsInit();
    if (!fn) return;
    char full[MAX_PATH];
    for (int i = 0; i < g_modN; i++) {
        if (!g_mods[i].enabled) continue;
        if (ModHas(&g_mods[i], relative, full, sizeof(full)))
            fn(full, g_mods[i].name, ctx);
    }
}

bool SWSE_FindModFile(const char* relative, char* out, int outLen) {
    SWSE_ModsInit();
    bool found = false;
    char full[MAX_PATH];
    // Walk forward and keep overwriting: the LAST enabled provider wins, which
    // is what "later mods win conflicts" means in load_order.txt.
    for (int i = 0; i < g_modN; i++) {
        if (!g_mods[i].enabled) continue;
        if (ModHas(&g_mods[i], relative, full, sizeof(full))) {
            lstrcpynA(out, full, outLen);
            found = true;
        }
    }
    return found;
}

int SWSE_CountModFile(const char* relative) {
    SWSE_ModsInit();
    int n = 0;
    for (int i = 0; i < g_modN; i++)
        if (g_mods[i].enabled && ModHas(&g_mods[i], relative, nullptr, 0)) n++;
    return n;
}
