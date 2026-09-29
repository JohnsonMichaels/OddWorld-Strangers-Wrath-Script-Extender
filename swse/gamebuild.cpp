// SWSE game-build identity - see gamebuild.h.

#include "gamebuild.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

// Fingerprints of the builds SWSE's addresses were measured on. Read from the
// PE header, which ASLR does not relocate.
struct KnownBuild {
    const char* name;
    DWORD timeDateStamp, sizeOfImage, entryPoint;
};
static const KnownBuild kKnown[] = {
    // Steam, bin\stranger.exe, 4,682,240 bytes (the build every RVA in SWSE
    // was measured on).
    { "Steam HD", 0x508AA980, 0x006BB000, 0x002E8CA8 },
};

static int   g_state = -1;          // -1 not checked, 0 unknown, 1 known
static int   g_known = -1;          // index into kKnown, -1 while taken as unknown
static int   g_match = -1;          // what the fingerprint matched, pretend or not
static DWORD g_ts = 0, g_size = 0, g_entry = 0;
static bool  g_override = false;    // SWSE_UNKNOWN_BUILD_OK=1
static bool  g_pretend = false;     // SWSE_PRETEND_UNKNOWN_BUILD=1

static bool EnvIs1(const char* name) {
    char v[8] = { 0 };
    return GetEnvironmentVariableA(name, v, sizeof(v)) && v[0] == '1';
}

static void Check() {
    if (g_state >= 0) return;
    g_state = 0;
    // The switches first: a header that cannot be read must not lose them.
    g_override = EnvIs1("SWSE_UNKNOWN_BUILD_OK");
    g_pretend  = EnvIs1("SWSE_PRETEND_UNKNOWN_BUILD");
    __try {
        BYTE* base = (BYTE*)GetModuleHandleA(NULL);
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        g_ts    = nt->FileHeader.TimeDateStamp;
        g_size  = nt->OptionalHeader.SizeOfImage;
        g_entry = nt->OptionalHeader.AddressOfEntryPoint;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (int i = 0; i < (int)(sizeof(kKnown) / sizeof(kKnown[0])); i++) {
        if (kKnown[i].timeDateStamp == g_ts && kKnown[i].sizeOfImage == g_size &&
            kKnown[i].entryPoint == g_entry) { g_match = i; break; }
    }
    // Pretending leaves the build unknown in every respect SWSE acts on - the
    // gates, `status`, `query version` - so the whole unknown path is tested.
    if (g_match >= 0 && !g_pretend) { g_known = g_match; g_state = 1; }
}

int SWSE_GameBuildKnown() {
    Check();
    return (g_state == 1 || g_override) ? 1 : 0;
}

int SWSE_GameBuildSafeMode() { return SWSE_GameBuildKnown() ? 0 : 1; }

const char* SWSE_GameBuildName() {
    Check();
    return (g_known >= 0) ? kKnown[g_known].name : "unknown";
}

void SWSE_GameBuildDescribe(char* out, int outLen) {
    Check();
    const char* pretend = "";
    char pb[120] = "";
    if (g_pretend && g_match >= 0) {
        _snprintf_s(pb, sizeof(pb), _TRUNCATE,
                    " - really %s, taken as unknown (SWSE_PRETEND_UNKNOWN_BUILD=1)",
                    kKnown[g_match].name);
        pretend = pb;
    }
    _snprintf_s(out, outLen, _TRUNCATE,
                "game build: %s (link stamp %08X, image %08X, entry %08X)%s%s",
                SWSE_GameBuildName(), g_ts, g_size, g_entry, pretend,
                g_state == 1 ? "" : g_override
                    ? " - UNKNOWN, but SWSE_UNKNOWN_BUILD_OK=1 overrides safe mode"
                    : " - UNKNOWN: safe mode, nothing that patches, calls or reads the game runs");
}

void SWSE_GameBuildRefusal(const char* what, char* out, int outLen) {
    _snprintf_s(out, outLen, _TRUNCATE,
                "%s is disabled on this game build (%s - safe mode): it uses addresses "
                "measured on the Steam release, and using them here could crash the game",
                what ? what : "this", SWSE_GameBuildName());
}
