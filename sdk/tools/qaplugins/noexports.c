/* noexports.c - a DLL that is not an SWSE plugin: no SWSEPlugin_Query or
   SWSEPlugin_Load export. Built x86 it is qanoexp.dll ("not an SWSE plugin");
   built x64 it is qa64.dll ("could not load the DLL (error 193...)"). SWSE
   must refuse both with the message of PLUGIN_SYSTEM.md 3.3 (test T13). */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
