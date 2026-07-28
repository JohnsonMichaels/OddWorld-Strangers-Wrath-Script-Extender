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

// Forward every dinput8 export to the real system DLL (copied beside us as
// dinput8_real.dll). Pragma-based forwarders are the robust way to do this -
// no .def, no stub functions, no name-decoration issues.
// DirectInput8Create is NOT forwarded: input.cpp implements it so SWSE can see
// (and inject into) every device read. Forwarding it here would win over that
// export and silently disable input injection.
#pragma comment(linker, "/export:DllCanUnloadNow=dinput8_real.DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/export:DllGetClassObject=dinput8_real.DllGetClassObject,PRIVATE")
#pragma comment(linker, "/export:DllRegisterServer=dinput8_real.DllRegisterServer,PRIVATE")
#pragma comment(linker, "/export:DllUnregisterServer=dinput8_real.DllUnregisterServer,PRIVATE")
#pragma comment(linker, "/export:GetdfDIJoystick=dinput8_real.GetdfDIJoystick")

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
