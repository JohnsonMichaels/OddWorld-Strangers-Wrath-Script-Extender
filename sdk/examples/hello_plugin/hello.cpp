// hello.cpp - the smallest useful SWSE plugin.
//
// What it shows:
//   * the two exports SWSE looks for: SWSEPlugin_Query and SWSEPlugin_Load;
//   * a console command, `hello [name]` - which also works from the remote
//     mailbox, a key bind (`bind F6 hello`), an alias or a trigger's `do =`;
//   * a per-frame callback (a frame counter);
//   * an event (a line in the console whenever a level comes up);
//   * the switch every plugin gets from its file name: `features hello on|off`.
//
// Install: <game>\SWSEMods\SWSE Hello\plugins\hello.dll, then type
// `features hello on` in the console (SWSE saves it to features.txt), then
// `hello`. Until that switch is on, SWSE does not even load the DLL.

#include "swse_plugin_api.h"

static SWSEPluginHandle      g_self;
static const SWSELogAPI*     g_log;
static const SWSEConsoleAPI* g_con;
static const SWSEGameAPI*    g_game;      // optional: NULL on an SWSE without it

static uint32_t g_frames;                 // frames seen while switched on
static uint32_t g_levels;                 // levels that came up while switched on

// ---- per frame ----------------------------------------------------------------
// TICK runs once per frame on the render thread. Keep it cheap: SWSE times it,
// and a slow frame is attributed to the plugin that caused it.
static void SWSE_CALL OnTick(const SWSEFrameInfo* frame, void* user) {
    (void)frame; (void)user;
    g_frames++;
}

// ---- a level came up ----------------------------------------------------------
static void SWSE_CALL OnLevelUp(uint32_t eventId, const void* data, void* user) {
    (void)eventId; (void)user;
    const SWSELevelEvent* ev = (const SWSELevelEvent*)data;
    g_levels++;
    // Unsolicited output: say who is talking.
    SWSE_Printf(g_con, g_self, "hello: a level is up (epoch %u) - %u frames so far",
                ev ? (unsigned)ev->epoch : 0u, (unsigned)g_frames);
}

// ---- the command ----------------------------------------------------------------
static void SWSE_CALL Cmd_Hello(int32_t argc, const char* const* argv, void* user) {
    (void)user;
    const char* who = (argc > 1) ? argv[1] : "Stranger";
    SWSE_Printf(g_con, g_self, "hello, %s! %u frames and %u level(s) seen since `features hello on`",
                who, (unsigned)g_frames, (unsigned)g_levels);
    if (!g_game) return;
    float p[3];
    if (g_game->PlayerPos(p) == SWSE_OK)
        SWSE_Printf(g_con, g_self, "  you are at %.1f %.1f %.1f", p[0], p[1], p[2]);
    else
        SWSE_Printf(g_con, g_self, "  (no level loaded, so no position)");
}

// ---- the switch -------------------------------------------------------------------
// onEnable starts the effect, onDisable undoes it. Load only registers things.
static int32_t SWSE_CALL OnEnable(char* msg, int32_t msgLen, void* user) {
    (void)user;
    g_frames = g_levels = 0;
    SWSE_Msg(msg, msgLen, "hello on - type `hello` or `hello <name>`");
    return 1;
}

static int32_t SWSE_CALL OnDisable(char* msg, int32_t msgLen, void* user) {
    (void)user;
    SWSE_Msg(msg, msgLen, "hello off after %u frames", (unsigned)g_frames);
    return 1;
}

// ---- the two exports -----------------------------------------------------------------
// Query: describe yourself and change nothing else.
SWSE_PLUGIN_EXPORT int32_t SWSE_CALL SWSEPlugin_Query(const SWSEInterface* swse,
                                                       SWSEPluginInfo* info) {
    (void)swse;
    info->apiVersion     = SWSE_PLUGIN_API_VERSION;
    info->name           = "hello";            // must match the file name, hello.dll
    info->version        = SWSE_MAKE_VERSION(1, 0, 0, 0);
    info->minSwseVersion = SWSE_MAKE_VERSION(1, 1, 0, 0);
    info->description    = "example plugin: a hello command, a frame counter, a level-up message";
    info->author         = "SWSE SDK";
    return 1;
}

// Load: fetch the tables you need, register, return 1.
SWSE_PLUGIN_EXPORT int32_t SWSE_CALL SWSEPlugin_Load(const SWSEInterface* swse) {
    g_self = swse->self;
    g_log  = SWSE_GET(swse, SWSELogAPI,     SWSE_IFACE_LOG,     1);
    g_con  = SWSE_GET(swse, SWSEConsoleAPI, SWSE_IFACE_CONSOLE, 1);
    g_game = SWSE_GET(swse, SWSEGameAPI,    SWSE_IFACE_GAME,    1);
    const SWSEFrameAPI*    frame = SWSE_GET(swse, SWSEFrameAPI,    SWSE_IFACE_FRAME,    1);
    const SWSEEventsAPI*   ev    = SWSE_GET(swse, SWSEEventsAPI,   SWSE_IFACE_EVENTS,   1);
    const SWSEFeaturesAPI* feat  = SWSE_GET(swse, SWSEFeaturesAPI, SWSE_IFACE_FEATURES, 1);
    if (!g_log || !g_con || !frame || !ev || !feat) return 0;    // an SWSE too old for us

    SWSECommandDesc cmd = { 0 };
    cmd.size = sizeof(cmd);
    cmd.name = "hello";
    cmd.help = "hello [name] - say hello; frames and levels seen";
    cmd.fn   = Cmd_Hello;
    SWSEStatus st = g_con->RegisterCommand(g_self, &cmd);
    if (st != SWSE_OK) {
        // SWSE_E_TAKEN: a built-in or an earlier plugin already has `hello`.
        SWSE_Logf(g_log, g_self, SWSE_LOG_ERROR, "could not register `hello` (status %d)", (int)st);
        return 0;                                  // SWSE rolls back what we registered
    }
    if (frame->Register(g_self, SWSE_FRAME_TICK, OnTick, NULL) != SWSE_OK) return 0;
    if (ev->Subscribe(g_self, SWSE_EV_LEVEL_UP, OnLevelUp, NULL) != SWSE_OK) return 0;
    feat->SetHandlers(g_self, OnEnable, OnDisable, NULL);

    // Functions newer than the version you asked for: test before calling.
    //   if (SWSE_HAS(g_con, SWSEConsoleAPI, Post)) g_con->Post(g_self, "status");

    SWSE_Logf(g_log, g_self, SWSE_LOG_INFO, "loaded into SWSE %u.%u (plugin API %u)",
              (unsigned)SWSE_VERSION_MAJOR(swse->swseVersion),
              (unsigned)SWSE_VERSION_MINOR(swse->swseVersion), (unsigned)swse->apiVersion);
    return 1;
}
