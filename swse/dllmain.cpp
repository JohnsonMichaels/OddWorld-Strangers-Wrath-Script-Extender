// SWSE - Stranger's Wrath Script Extender
// Proxy dinput8.dll: injects into stranger.exe, forwards input to the real
// dinput8, and bootstraps SWSE. Graphics/effects are NOT built in here - SWSE
// discovers enabled "graphics" mods under SWSEMods\ and will load their
// shaders. This keeps "SWSE Graphics" a separate, user-loadable mod.
//
// M1 (this file): prove injection + prove graphics-mod discovery, by writing
// swse_log.txt next to the game exe listing what SWSE found. The wglSwapBuffers
// frame hook + shader passes land in M2+.

#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include "framehook.h"

static void Log(const std::string& line);   // defined below

// ---- forwarding the real dinput8 -----------------------------------------
//
// These used to be LINK-TIME forwards to `dinput8_real.dll`. That made a file
// the user had to create by hand load-bearing, and it failed in the worst
// possible way: a forward is resolved LAZILY, at the first call, so the DLL
// loaded fine and wrote its log, and then the process died the instant the
// game called DirectInput8Create. No window, no error, a log file that proved
// SWSE "worked". Both of the first two people to install SWSE hit this, from
// opposite directions - one never created the file, the other created it by
// copying the system DLL over SWSE's own.
//
// Now they are ordinary functions that resolve at runtime and, if
// dinput8_real.dll is absent, fall back to the real system dinput8.dll by
// absolute path. dinput8_real.dll becomes optional, and a missed install step
// is no longer fatal.
//
// Resolution order, and why:
//   1. an already-loaded dinput8_real.dll   (legacy installs keep working)
//   2. dinput8_real.dll beside our own DLL  (by path, never by bare name)
//   3. %SystemDirectory%\dinput8.dll        (the real thing; for a 32-bit
//                                            process GetSystemDirectory
//                                            returns SysWOW64, which is right)
// A bare LoadLibraryA("dinput8.dll") is never used: we ARE dinput8.dll, and
// that would bind us to ourselves.
extern "C" HMODULE SWSE_RealDInput8() {
    static HMODULE cached = nullptr;
    if (cached) return cached;

    cached = GetModuleHandleA("dinput8_real.dll");
    if (!cached) {
        char path[MAX_PATH];
        HMODULE self = GetModuleHandleA("dinput8.dll");
        if (self && GetModuleFileNameA(self, path, MAX_PATH)) {
            char* slash = strrchr(path, '\\');
            if (slash) {
                lstrcpyA(slash + 1, "dinput8_real.dll");
                cached = LoadLibraryA(path);
            }
        }
    }
    if (!cached) {
        char sys[MAX_PATH];
        UINT n = GetSystemDirectoryA(sys, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 16) {
            lstrcatA(sys, "\\dinput8.dll");
            cached = LoadLibraryA(sys);
            if (cached) Log("input: using the system dinput8.dll "
                            "(dinput8_real.dll not present - that is fine)");
        }
    }
    if (!cached) Log("input: FATAL - could not load any real dinput8.dll");
    return cached;
}

// Resolve one export from the real DLL, once.
static FARPROC RealProc(const char* name) {
    HMODULE m = SWSE_RealDInput8();
    return m ? GetProcAddress(m, name) : nullptr;
}

// The stubs. Undecorated export names are set below with /export aliases,
// because __stdcall decorates to _Name@N on x86 and COM callers look up the
// plain name.
//
// DirectInput8Create is deliberately NOT here: input.cpp implements it so SWSE
// can hook device reads. Defining it here as well would win over that export
// and silently disable input injection.
extern "C" HRESULT WINAPI DllCanUnloadNow(void) {
    typedef HRESULT (WINAPI *pfn)(void);
    static pfn p = (pfn)RealProc("DllCanUnloadNow");
    return p ? p() : S_FALSE;          // S_FALSE = "do not unload me"
}
extern "C" HRESULT WINAPI DllGetClassObject(const GUID& rclsid, const GUID& riid, void** ppv) {
    typedef HRESULT (WINAPI *pfn)(const GUID&, const GUID&, void**);
    static pfn p = (pfn)RealProc("DllGetClassObject");
    return p ? p(rclsid, riid, ppv) : E_FAIL;
}
extern "C" HRESULT WINAPI DllRegisterServer(void) {
    typedef HRESULT (WINAPI *pfn)(void);
    static pfn p = (pfn)RealProc("DllRegisterServer");
    return p ? p() : E_FAIL;
}
extern "C" HRESULT WINAPI DllUnregisterServer(void) {
    typedef HRESULT (WINAPI *pfn)(void);
    static pfn p = (pfn)RealProc("DllUnregisterServer");
    return p ? p() : E_FAIL;
}
extern "C" void* WINAPI GetdfDIJoystick(void) {
    typedef void* (WINAPI *pfn)(void);
    static pfn p = (pfn)RealProc("GetdfDIJoystick");
    return p ? p() : nullptr;
}

#pragma comment(linker, "/export:DllCanUnloadNow=_DllCanUnloadNow@0,PRIVATE")
#pragma comment(linker, "/export:DllGetClassObject=_DllGetClassObject@12,PRIVATE")
#pragma comment(linker, "/export:DllRegisterServer=_DllRegisterServer@0,PRIVATE")
#pragma comment(linker, "/export:DllUnregisterServer=_DllUnregisterServer@0,PRIVATE")
#pragma comment(linker, "/export:GetdfDIJoystick=_GetdfDIJoystick@0")

static void Log(const std::string& line) {
    // log sits in the game's bin\ (our DLL's folder)
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    std::string p(path);
    std::string dir = p.substr(0, p.find_last_of("\\/"));
    std::ofstream f(dir + "\\swse_log.txt", std::ios::app);
    f << line << "\n";
}

// Enumerate enabled graphics mods under <game>\SWSEMods, honoring load_order.txt.
static std::vector<std::string> FindGraphicsMods() {
    char exe[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    std::string p(exe);
    std::string bin = p.substr(0, p.find_last_of("\\/"));      // ...\Stranger's Wrath\bin
    std::string root = bin.substr(0, bin.find_last_of("\\/")); // ...\Stranger's Wrath
    std::string mods = root + "\\SWSEMods";

    // read disabled set from load_order.txt (lines starting with '!')
    std::vector<std::string> disabled;
    std::ifstream order(mods + "\\load_order.txt");
    std::string ln;
    while (std::getline(order, ln)) {
        if (!ln.empty() && ln[0] == '!') disabled.push_back(ln.substr(1));
    }

    std::vector<std::string> found;
    std::string glob = mods + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string name = fd.cFileName;
        if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;
        // a graphics mod declares itself with a graphics.json
        std::string gj = mods + "\\" + name + "\\graphics.json";
        if (GetFileAttributesA(gj.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        bool off = false;
        for (auto& d : disabled) if (d == name) off = true;
        if (!off) found.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static void InitSWSE() {
    Log("==== SWSE injected ====");
    Log("Stranger's Wrath Script Extender loaded into stranger.exe");
    auto mods = FindGraphicsMods();
    if (mods.empty()) {
        Log("no enabled graphics mods found (drop one in SWSEMods\\ with a graphics.json)");
    } else {
        for (auto& m : mods) Log("graphics mod ready to load: " + m);
    }
    Log("installing frame hook (M2)...");
    SWSE_StartFrameHook();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitSWSE();
    }
    return TRUE;
}
