// SWSE native plugins (1.1) - the loader, the API tables it serves, and the
// dispatch of every call into a plugin.
//
// A plugin is a 32-bit DLL in SWSEMods\<Mod>\plugins\<name>.dll that exports
// SWSEPlugin_Query and SWSEPlugin_Load and talks to SWSE only through the
// tables in sdk\swse_plugin_api.h. The design of record, with the reasoning
// behind every rule here, is swse\research\PLUGIN_SYSTEM.md; the measured
// limits are in PLUGIN_QA.md.
//
// THE RULES THIS FILE ENFORCES
//   * A plugin is a feature switch named after its DLL, off unless
//     features.txt or `features <name> on` says otherwise - and while it is
//     off its DLL is never even loaded. `features all on` skips plugins (a DLL
//     runs only when named); `features all off` includes them.
//   * Load order is the mod registry's: SWSEMods\load_order.txt orders and
//     disables a plugin with its mod; two mods shipping one name, the later
//     mod's is used and the other is listed as shadowed.
//   * Every call into a plugin runs inside a structured-exception guard and is
//     timed. A fault switches that plugin off for the session and names the
//     DLL and offset; features.txt is left alone, so a fixed build loads next
//     launch. A frame stall names the plugin that caused it.
//   * A plugin is never unloaded: off means its callbacks stop.
//   * Everything runs on the render thread (the one presenting frames),
//     except the few functions the header marks [any thread].
//
// NO PLUGINS, NO CODE PATH. With no plugin switched on, every per-frame entry
// below returns after one test, and no hook is installed on a plugin's
// account: the GL notifications ride SWSE's own hooks, installed only when a
// plugin that is on asks for them.
#pragma once
#include <windows.h>

// ---- lifecycle (framehook.cpp) ---------------------------------------------------
// First frame, after SWSE_FeaturesInit: record the render thread and discover
// every plugin (each becomes a switch). Plugins that features.txt switches on
// are then started by the frame hook's start loop, after the built-ins.
void SWSE_PluginsInit();
// Top of every frame: roll the per-plugin timings, compact the registries (no
// plugin call is on the stack here), run lines queued with Console.Post.
void SWSE_PluginsFrameBegin(HDC hdc);
// After SWSE_LevelTick: LEVEL_UP / LEVEL_DOWN from the level watcher's epoch.
void SWSE_PluginsLevelEvents();
// Per-frame callbacks. 0 = TICK (after SWSE's own systems), 1 = OVERLAY (after
// the post-process, before the console, inside a GL state snapshot).
void SWSE_PluginsFrame(unsigned phase, HDC hdc);
// For the FRAMESTALL line: the plugin that owns most of this frame's SWSE
// time, if one owns more than half of `swseMs`. 1 and "plugin <name> <n> ms".
int  SWSE_PluginsStallCulprit(double swseMs, char* out, int outLen);

// ---- switches (framehook.cpp SWSE_FeatureStart/Stop) -------------------------------
// Switch on: load the DLL if this is its first time (Query, Load), then its
// onEnable. False (and why in msg) when it did not come on.
bool SWSE_PluginFeatureStart(int feat, char* msg, int msgLen);
bool SWSE_PluginFeatureStop(int feat, char* msg, int msgLen);
// One line about a plugin switch for `features`: "(plugin, not loaded) ...".
void SWSE_PluginDescribeFeature(int feat, char* out, int outLen);
// A switch changed live (`features`): the FEATURE event, to plugins that are on.
void SWSE_PluginsNotifyFeature(int feat, bool on);

// ---- console (console.cpp) ----------------------------------------------------------
// Run a plugin command if one is registered under argv[0]. True when the line
// was a plugin command's (run, or answered "belongs to plugin x, which is off").
bool SWSE_PluginCmdRun(int argc, char** argv);
bool SWSE_PluginCmdExists(const char* name);
// Tab completion: names starting with `prefix` that would run now.
int  SWSE_PluginCmdComplete(const char* prefix, const char** out, int maxOut);
// Enumerate registered commands for `help`: 1 while index i exists. `on` is 1
// when the command would run now.
int  SWSE_PluginCmdAt(int i, const char** name, const char** category,
                      const char** help, const char** plugin, int* on);
// `plugins [name|rescan]`, and the `query plugins` line.
void SWSE_PluginsCommand(int argc, char** argv);
void SWSE_PluginsQuery(char* out, int outLen);
// `mods reload`: find plugins added since launch (new switches, off), then
// MODS_RELOADED to plugins that are on.
void SWSE_PluginsRescan();
void SWSE_PluginsModsReloaded();
// Is `name` a plugin's (loaded or not)? For `help <plugin>`.
bool SWSE_PluginIsName(const char* name);
// How many plugins were found / are on.
int  SWSE_PluginsFound();
int  SWSE_PluginsOn();

// ---- reports (selftest.cpp, console.cpp perf/status) ---------------------------------
// selftest: a line per plugin that is on - its own checks when it registered a
// SELFTEST report, else SWSE's evidence that it runs - FAIL for one that
// faulted, and the names of those that are off for the [OFF ] line.
void SWSE_PluginsSelfTest(void (*check)(int ok, const char* name, const char* detail),
                          void (*off)(const char* name));
// perf: each loaded plugin's frame cost, then its PERF report lines.
void SWSE_PluginsPerf(void (*emit)(const char* line));
// status: a summary line, then STATUS report lines from plugins that are on.
void SWSE_PluginsStatus(void (*emit)(const char* line));

// ---- GL notifications (glspy.cpp) ------------------------------------------------------
// Fed from SWSE's own hooks - a plugin never patches GL itself, because the
// unhook-call-rehook hooks would erase its patch (PLUGIN_SYSTEM.md C1). Each
// is one test in the hook while no plugin that is on listens.
extern volatile int g_swsePluginBindListeners;     // >0: a listener that is on
extern volatile int g_swsePluginUploadListeners;   // >0: an upload subscriber that is on
// Inside glBindTexture, before the real bind; not re-entered for a bind a
// listener makes itself.
void SWSE_PluginsNotifyBind(unsigned target, unsigned tex);
// Inside glCompressedTexImage2D, for the level-0 uploads foliage fingerprints.
void SWSE_PluginsNotifyUpload(unsigned texId, unsigned fingerprint, int w, int h,
                              unsigned glFormat, int dataSize, const void* data);
