/*
 * swse_plugin_api.h - the SWSE plugin API, version 1
 *
 * SWSE, the Stranger's Wrath Script Extender, loads native plugins: 32-bit
 * DLLs placed in  SWSEMods\<Mod>\plugins\<name>.dll . A plugin exports two
 * functions, SWSEPlugin_Query and SWSEPlugin_Load, and talks to SWSE only
 * through the function tables in this header. It never links against SWSE
 * and never needs SWSE's source.
 *
 * STATUS
 *   Served by SWSE 1.1 (swse/plugins.cpp is compiled from this header; the
 *   design and its reasons are swse/research/PLUGIN_SYSTEM.md). Every table
 *   below is served, the console table at version 2 (with Post), except the
 *   ones marked [phase 4]: those are only compiled with
 *   SWSE_PLUGIN_API_PROPOSED defined, are here for review, and GetInterface()
 *   answers NULL for them. An event SWSE does not raise answers
 *   SWSE_E_UNSUPPORTED from Subscribe() - never a silent no-op.
 *
 * LIFECYCLE
 *   A plugin is OFF until the user switches it on. Its switch is its DLL name:
 *   `features hello on` in the console, or `hello = on` in
 *   SWSEMods\features.txt. While off, SWSE never even loads the DLL. On the
 *   first switch-on, on the game's render thread, SWSE calls
 *       SWSEPlugin_Query  - fill in SWSEPluginInfo, change nothing else;
 *       SWSEPlugin_Load   - register commands, callbacks and events;
 *       your onEnable     - start your effect (see SWSEFeaturesAPI).
 *   `features hello off` calls your onDisable and stops every callback; the
 *   DLL stays loaded until the game exits (SWSE never unloads a plugin, for
 *   the reason in PLUGIN_SYSTEM.md section 6.5). A later `features hello on`
 *   calls onEnable again - Load runs once per session.
 *
 * ABI RULES (the compatibility contract)
 *   - C linkage and __cdecl (SWSE_CALL) everywhere. 32-bit x86 only.
 *   - No C runtime object crosses the boundary: no memory allocated on one
 *     side is freed on the other, no FILE*, no C++ types, no exceptions. SWSE
 *     and each plugin may use different C runtimes (build yours with /MT).
 *   - Strings you pass in are NUL-terminated char and are copied by SWSE
 *     before the call returns. Strings SWSE gives back are written into your
 *     buffer (char* out, int32_t outLen), always NUL-terminated, cut if short.
 *     Paths use the ANSI code page (SWSE uses the A file APIs); console text
 *     is printable ASCII (the console font has nothing else).
 *   - Every struct starts with `uint32_t size`. A table's existing fields are
 *     never moved, removed or re-typed; new functions are appended and raise
 *     `version`. Test for a function newer than the table's version you asked
 *     for with SWSE_HAS(). A struct you hand to SWSE: set size = sizeof().
 *   - Status: SWSEStatus, SWSE_OK (0) or a negative SWSE_E_* code. Functions
 *     that return a count return >= 0 or a negative SWSE_E_* code.
 *   - Threads: every function is RENDER THREAD ONLY unless marked
 *     [any thread]; from another thread it returns SWSE_E_THREAD and does
 *     nothing. Every callback arrives on the render thread with the game's
 *     GL context current.
 *   - A fault (access violation, C++ exception, stack overflow...) inside any
 *     callback SWSE makes into your plugin is caught; the game keeps running,
 *     your plugin is switched off for the rest of the session, and the
 *     console and swse_log.txt name your DLL and the offset: of the faulting
 *     instruction, of the code that threw, or of your last call on the stack
 *     when the fault happened inside something you called. NOT caught
 *     (measured, PLUGIN_QA.md): a fault on a thread you started - guard its
 *     top frame yourself; heap corruption, which returns normally and
 *     crashes the game later; and a callback that never returns.
 */
#ifndef SWSE_PLUGIN_API_H
#define SWSE_PLUGIN_API_H

#include <stddef.h>   /* offsetof */
#include <stdint.h>

#if !defined(_M_IX86) && !defined(__i386__)
#error "SWSE plugins are 32-bit x86 DLLs (stranger.exe is a 32-bit process): build with the x86 toolchain (vcvars32.bat)."
#endif

#define SWSE_CALL __cdecl

#ifdef __cplusplus
#define SWSE_EXTERN_C extern "C"
#else
#define SWSE_EXTERN_C
#endif

/* Put this in front of your two exports. */
#define SWSE_PLUGIN_EXPORT SWSE_EXTERN_C __declspec(dllexport)

#if defined(__cplusplus)
#define SWSE_INLINE inline
#elif defined(_MSC_VER)
#define SWSE_INLINE __inline
#else
#define SWSE_INLINE inline
#endif

#pragma pack(push, 8)

/* ==== versions ============================================================ */

/* 1.1.0.0 -> 0x01010000. Used for SWSE's version and for yours. */
#define SWSE_MAKE_VERSION(major, minor, patch, build)                        \
    ((((uint32_t)(major) & 0xFFu) << 24) | (((uint32_t)(minor) & 0xFFu) << 16) | \
     (((uint32_t)(patch) & 0xFFu) << 8)  |  ((uint32_t)(build) & 0xFFu))
#define SWSE_VERSION_MAJOR(v) (((uint32_t)(v) >> 24) & 0xFFu)
#define SWSE_VERSION_MINOR(v) (((uint32_t)(v) >> 16) & 0xFFu)
#define SWSE_VERSION_PATCH(v) (((uint32_t)(v) >> 8) & 0xFFu)

/* The plugin API this header describes. Raised only by a breaking change,
   which is not planned; growth happens by appending to tables instead. */
#define SWSE_PLUGIN_API_VERSION 1u

/* ==== basic types and status codes ======================================= */

/* Identifies your plugin to SWSE. Pass swse->self wherever one is asked for. */
typedef uint32_t SWSEPluginHandle;
typedef int32_t  SWSEStatus;

#define SWSE_OK               0
#define SWSE_E_BADARG       (-1)   /* NULL/empty/invalid argument or name       */
#define SWSE_E_NOTFOUND     (-2)   /* no such command, event, label, file...    */
#define SWSE_E_TAKEN        (-3)   /* a built-in's name, one you hold already,
                                      or an address someone already patched    */
#define SWSE_E_FULL         (-4)   /* a fixed-size SWSE table is full           */
#define SWSE_E_NOTREADY     (-5)   /* no level, no player, nothing hooked yet   */
#define SWSE_E_FAULT        (-6)   /* the access faulted; nothing crashed       */
#define SWSE_E_THREAD       (-7)   /* render-thread-only function, wrong thread */
#define SWSE_E_OFF          (-8)   /* your switch is off, or you were disabled  */
#define SWSE_E_UNSUPPORTED  (-9)   /* this SWSE does not implement it, or not
                                      on this game build (safe mode: SWSE
                                      does not recognise the exe, and keeps
                                      out of the game). Final for the session */
#define SWSE_E_BADHANDLE   (-10)   /* not a valid SWSEPluginHandle              */
#define SWSE_E_NESTING     (-11)   /* console commands nested more than 8 deep  */
#define SWSE_E_BUSY        (-12)   /* not allowed from inside a GL hook callback
                                      (bind listener, texture upload event)     */

/* ==== the two exports ===================================================== */

#define SWSE_PLUGIN_F_NONE        0x0u
/* [phase 4] A promise that the plugin installs no code patches of its own,
   starts no threads and keeps no GL objects past onDisable, so a developer
   build of SWSE may unload and reload it. Ignored until phase 4. */
#define SWSE_PLUGIN_F_RELOADABLE  0x1u

/* Filled in by SWSEPlugin_Query. SWSE zeroes it and sets `size` first; write
   only fields that fit (all of these do, for API version 1). The strings must
   stay valid until Query returns - SWSE copies them. */
typedef struct SWSEPluginInfo {
    uint32_t    size;            /* set by SWSE                                   */
    uint32_t    apiVersion;      /* set to SWSE_PLUGIN_API_VERSION                */
    const char* name;            /* your DLL's file name without ".dll": lower case
                                    a-z 0-9 _ -, 1..31 chars. It is also your
                                    switch in features.txt. Must match the file. */
    uint32_t    version;         /* your version, SWSE_MAKE_VERSION(...)          */
    uint32_t    minSwseVersion;  /* the oldest SWSE you work with                 */
    const char* description;     /* one line, shown by `features` and `plugins`   */
    const char* author;          /* optional                                      */
    const char* aliases;         /* optional, comma-separated console nicknames
                                    for your switch, e.g. "wind,grass,plants".
                                    features.txt only reads the real name.       */
    uint32_t    flags;           /* SWSE_PLUGIN_F_*                               */
} SWSEPluginInfo;

struct SWSEInterface;

/* Return 1 to load, 0 to refuse (log why first - the Log table works here).
   Do nothing else in Query: no registrations, no game or GL calls. */
typedef int32_t (SWSE_CALL *SWSEPlugin_QueryFn)(const struct SWSEInterface* swse,
                                                SWSEPluginInfo* info);
/* Register what you need, return 1. Return 0 (or fault) and SWSE rolls back
   every registration you made and leaves your switch off. Registering is all
   Load should do: start your effect in onEnable. */
typedef int32_t (SWSE_CALL *SWSEPlugin_LoadFn)(const struct SWSEInterface* swse);

#define SWSE_PLUGIN_QUERY_NAME "SWSEPlugin_Query"
#define SWSE_PLUGIN_LOAD_NAME  "SWSEPlugin_Load"

/* ==== the core interface ================================================== */

/* Interface ids for GetInterface. */
#define SWSE_IFACE_LOG        1u   /* SWSELogAPI       - SWSE 1.1              */
#define SWSE_IFACE_CONSOLE    2u   /* SWSEConsoleAPI   - SWSE 1.1 (version 2)  */
#define SWSE_IFACE_FRAME      3u   /* SWSEFrameAPI     - SWSE 1.1              */
#define SWSE_IFACE_EVENTS     4u   /* SWSEEventsAPI    - SWSE 1.1              */
#define SWSE_IFACE_FEATURES   5u   /* SWSEFeaturesAPI  - SWSE 1.1              */
#define SWSE_IFACE_MODS       6u   /* SWSEModsAPI      - SWSE 1.1              */
#define SWSE_IFACE_GAME       7u   /* SWSEGameAPI      - SWSE 1.1              */
#define SWSE_IFACE_MEMORY     8u   /* SWSEMemoryAPI    - SWSE 1.1              */
#define SWSE_IFACE_GL         9u   /* SWSEGLAPI        - SWSE 1.1              */
#define SWSE_IFACE_REPORTS   10u   /* SWSEReportsAPI   - SWSE 1.1              */
#define SWSE_IFACE_PREFS     11u   /* SWSEPrefsAPI     - phase 4 (proposed)    */
#define SWSE_IFACE_SCRIPT    12u   /* SWSEScriptAPI    - phase 4 (proposed)    */
#define SWSE_IFACE_HOOKS     13u   /* SWSEHooksAPI     - phase 4 (proposed)    */

/* Handed to Query and Load; valid (same pointer) for the whole session. */
typedef struct SWSEInterface {
    uint32_t         size;
    uint32_t         apiVersion;    /* the plugin API SWSE implements (1)       */
    uint32_t         swseVersion;   /* the running SWSE, SWSE_MAKE_VERSION      */
    SWSEPluginHandle self;          /* your handle                              */
    /* [any thread] The table for `id` if SWSE implements version >= minVersion,
       else NULL. During Query only SWSE_IFACE_LOG is served. Tables are
       static: cache the pointers. */
    const void* (SWSE_CALL *GetInterface)(uint32_t id, uint32_t minVersion);
} SWSEInterface;

/* ==== log ================================================================= */

#define SWSE_LOG_INFO   0
#define SWSE_LOG_WARN   1
#define SWSE_LOG_ERROR  2

typedef struct SWSELogAPI {
    uint32_t size;
    uint32_t version;              /* 1 */
    /* [any thread] Append "[<plugin>] <text>" to bin\swse_log.txt ("WARN: "
       or "ERROR: " after the tag for those levels). The file is opened and
       closed per line, so the last line before a crash survives. */
    void (SWSE_CALL *Write)(SWSEPluginHandle self, int32_t level, const char* text);
} SWSELogAPI;

/* ==== console ============================================================= */

/* argv[0] is the command name as typed; arguments are split on spaces and
   tabs (no quoting), at most 32 words. argv is valid only during the call. */
typedef void (SWSE_CALL *SWSECommandFn)(int32_t argc, const char* const* argv, void* user);

/* Runs even while your switch is off (a status or help command, say).
   Without it SWSE answers "<cmd> belongs to <plugin>, which is off". */
#define SWSE_CMD_F_WHILE_OFF  0x1u

typedef struct SWSECommandDesc {
    uint32_t      size;       /* sizeof(SWSECommandDesc)                        */
    const char*   name;       /* 1..31 chars of a-z 0-9 _ - . (case-insensitive) */
    const char*   category;   /* `help` heading; NULL = your plugin's name       */
    const char*   help;       /* one line: "hello [name] - say hello"           */
    SWSECommandFn fn;
    void*         user;       /* handed back to fn                              */
    uint32_t      flags;      /* SWSE_CMD_F_*                                   */
} SWSECommandDesc;

typedef struct SWSEConsoleAPI {
    uint32_t size;
    uint32_t version;         /* 2 in SWSE 1.1 (version 2 added Post) */
    /* Print to the console, one line per '\n'; each line is cut at 239
       characters. While a command runs from the remote mailbox its output is
       also captured into remote_out.txt, so tools see what you print here -
       and nothing you only write to the log. With the console switched off
       the text goes nowhere visible: log anything that matters. */
    void       (SWSE_CALL *Print)(SWSEPluginHandle self, const char* text);
    /* Add a command. SWSE_E_TAKEN if it is a built-in command's name (those
       can never be replaced) or you registered it already. Another plugin
       may offer the same name: both registrations stand, and while both
       plugins are on, the one LATER in load order runs - the rule
       load_order.txt states for every conflict ("later mods win"); `help
       <name>` lists every provider. So the outcome never depends on which
       plugin happened to be switched on first. A typed line is resolved:
       built-in, plugin command, scripts\*.txt, alias, then one of the game's
       own script functions. Your commands also work from the remote mailbox,
       key binds, aliases and trigger `do =` lines. */
    SWSEStatus (SWSE_CALL *RegisterCommand)(SWSEPluginHandle self, const SWSECommandDesc* desc);
    SWSEStatus (SWSE_CALL *UnregisterCommand)(SWSEPluginHandle self, const char* name);
    /* Run a line exactly as if typed: ';' sequences, `wait`, aliases, scripts,
       game functions. Every console command is an action you can take this
       way ("grant surgerybid 1", "warp 3", "prefs set ..."). Returns once the
       line has run (a `wait` defers the rest to later frames); the command's
       own output goes to the console, not back to you. SWSE_E_NESTING past
       8 levels of commands running commands. */
    SWSEStatus (SWSE_CALL *Execute)(SWSEPluginHandle self, const char* line);
    /* 1 while the console overlay is open: ignore your own hotkeys then. */
    int32_t    (SWSE_CALL *IsOpen)(void);

    /* ---- version 2 ---- */
    /* [any thread] Queue a line to run on the render thread at the start of
       the next frame - the way back from a worker thread you started. Lines
       run in the order posted, and only while your switch is on;
       SWSE_E_FULL when 64 are already waiting. */
    SWSEStatus (SWSE_CALL *Post)(SWSEPluginHandle self, const char* line);
} SWSEConsoleAPI;

/* ==== per-frame callbacks ================================================= */

/* TICK: once per frame, just before the frame is presented, after SWSE's own
   per-frame systems (level watcher, tuning, triggers) and before the
   post-process. For logic. The game's GL context is current: restore every
   GL binding and enable you change.
   OVERLAY: once per frame after the post-process and before the console
   overlay, so the console stays on top. For drawing. SWSE saves and restores
   the attribute stacks and matrices around this phase, and puts back the
   program, framebuffer and buffer bindings if they changed - restore your
   own anyway: the plugins after you share this phase. */
#define SWSE_FRAME_TICK     0u
#define SWSE_FRAME_OVERLAY  1u

typedef struct SWSEFrameInfo {
    uint32_t size;
    uint32_t phase;          /* SWSE_FRAME_TICK or SWSE_FRAME_OVERLAY           */
    uint32_t frame;          /* frames since SWSE started                       */
    uint32_t tickMs;         /* GetTickCount() at the start of this frame       */
    float    dtMs;           /* milliseconds since the previous frame           */
    int32_t  width;          /* game window client area                         */
    int32_t  height;
    void*    hdc;            /* the HDC being presented                         */
    uint32_t levelUp;        /* 1 while a level is up (a player body exists)    */
    uint32_t levelEpoch;     /* see SWSE_EV_LEVEL_UP                            */
    uint32_t consoleOpen;    /* 1 while the console overlay is open             */
} SWSEFrameInfo;

typedef void (SWSE_CALL *SWSEFrameFn)(const SWSEFrameInfo* frame, void* user);

typedef struct SWSEFrameAPI {
    uint32_t size;
    uint32_t version;        /* 1 */
    /* Called every frame while your switch is on. SWSE times each callback;
       `plugins` shows the cost and a frame stall names the plugin that
       caused it. Registering from inside a callback takes effect next frame. */
    SWSEStatus (SWSE_CALL *Register)(SWSEPluginHandle self, uint32_t phase, SWSEFrameFn fn, void* user);
    SWSEStatus (SWSE_CALL *Unregister)(SWSEPluginHandle self, uint32_t phase, SWSEFrameFn fn, void* user);
    /* [any thread] 1 on the render thread (the one that presents frames). */
    int32_t    (SWSE_CALL *IsRenderThread)(void);
} SWSEFrameAPI;

/* ==== events ============================================================== */

/* A new player body appeared: a level finished loading (also a checkpoint
   reload, and anything else that rebuilds the player's motion object - so it
   can arrive without a LEVEL_DOWN before it). data: const SWSELevelEvent*.
   The level's cast may still be spawning; SWSEGameAPI.LevelDue waits for a
   settle delay, which is what SWSE's own per-level appliers use (3-5 s). */
#define SWSE_EV_LEVEL_UP         1u
/* The player body has been gone 1.5 s: the menu, or a load that draws frames
   that long. A load within a frame or two (a warp, often) raises only the next
   LEVEL_UP. data: const SWSELevelEvent*, the epoch that ended.              */
#define SWSE_EV_LEVEL_DOWN       2u
/* `mods reload` ran: re-read your files. data: NULL. */
#define SWSE_EV_MODS_RELOADED    3u
/* Any switch changed live (built-in or plugin): `features`, `auto`, or a
   plugin switched off by its fault. data: const SWSEFeatureEvent*. */
#define SWSE_EV_FEATURE          4u
/* A level-0 compressed texture was uploaded. Delivered INSIDE the
   upload hook, synchronously, for every upload: keep it cheap (Execute and
   (un)registering return SWSE_E_BUSY there). Subscribing installs SWSE's
   upload hook. data: const SWSETextureUploadEvent*. */
#define SWSE_EV_TEXTURE_UPLOAD  16u

typedef struct SWSELevelEvent {
    uint32_t size;
    uint32_t epoch;          /* increments once per new player body             */
    uint32_t player;         /* player object address; 0 for LEVEL_DOWN        */
    uint32_t motion;         /* the motion object that defines this epoch       */
} SWSELevelEvent;

typedef struct SWSEFeatureEvent {
    uint32_t    size;
    const char* name;        /* canonical switch name: "graphics", "foliage"... */
    uint32_t    on;
} SWSEFeatureEvent;

typedef struct SWSETextureUploadEvent {
    uint32_t    size;
    uint32_t    texId;       /* the GL texture the data landed in. GL recycles ids
                                across levels: trust a flag keyed on it only until
                                the next upload into the same id.                */
    uint32_t    fingerprint; /* SWSE's texture fingerprint of the vanilla data -
                                tools/texmap.py computes the same value from the
                                game archives, where the artists' names live.   */
    int32_t     width;
    int32_t     height;
    uint32_t    glFormat;    /* compressed internal format (GL enum)            */
    int32_t     dataSize;
    const void* data;        /* the level-0 data; valid only during the callback */
} SWSETextureUploadEvent;

typedef void (SWSE_CALL *SWSEEventFn)(uint32_t eventId, const void* data, void* user);

typedef struct SWSEEventsAPI {
    uint32_t size;
    uint32_t version;        /* 1 */
    /* Delivered only while your switch is on. SWSE_E_UNSUPPORTED if this
       SWSE does not raise that event (yet) - test the return value. Safe to
       call from inside any callback, your own included: an unsubscribe takes
       effect at once, a subscription from the next time the event is raised
       (the same rule as frame callbacks, and as commands and listeners). */
    SWSEStatus (SWSE_CALL *Subscribe)(SWSEPluginHandle self, uint32_t eventId, SWSEEventFn fn, void* user);
    SWSEStatus (SWSE_CALL *Unsubscribe)(SWSEPluginHandle self, uint32_t eventId, SWSEEventFn fn, void* user);
} SWSEEventsAPI;

/* ==== your switch ========================================================= */

/* Write one line for the console into msg (e.g. "wind on - 42 plants known")
   and return 1. onEnable returning 0 (or faulting) leaves the switch off. */
typedef int32_t (SWSE_CALL *SWSESwitchFn)(char* msg, int32_t msgLen, void* user);

typedef struct SWSEFeaturesAPI {
    uint32_t size;
    uint32_t version;        /* 1 */
    /* onEnable: at launch when features.txt says `<name> = on` (right after
       Load), or on `features <name> on`. onDisable: `features <name> off`.
       Undo what you changed in onDisable - "off" should mean the game runs
       as if you were not there. Either may be NULL. */
    SWSEStatus (SWSE_CALL *SetHandlers)(SWSEPluginHandle self, SWSESwitchFn onEnable,
                                        SWSESwitchFn onDisable, void* user);
    /* Any switch, built-in ("graphics", "hdtextures"...) or a plugin's, by
       name or console nickname: 1 on, 0 off, -1 no such switch. */
    int32_t    (SWSE_CALL *IsOn)(const char* name);
} SWSEFeaturesAPI;

/* ==== mod folders ========================================================= */

/* A mod is a folder under SWSEMods\; SWSEMods\load_order.txt orders them and
   "!Name" disables one. ADDITIVE files: read every enabled mod's copy, in
   order. LAST-WINS files: the last enabled mod's copy decides. */
typedef void (SWSE_CALL *SWSEModFileFn)(const char* absPath, const char* modName, void* ctx);

typedef struct SWSEModsAPI {
    uint32_t size;
    uint32_t version;        /* 1 */
    /* ADDITIVE: fn for every enabled mod providing `relative` (a file such as
       "foliage.txt" or a folder such as "textures"), in load order. */
    void       (SWSE_CALL *ForEachModFile)(const char* relative, SWSEModFileFn fn, void* ctx);
    /* LAST WINS: the last enabled mod providing `relative`. 1 found, 0 not. */
    int32_t    (SWSE_CALL *FindModFile)(const char* relative, char* out, int32_t outLen);
    /* How many enabled mods provide `relative`. */
    int32_t    (SWSE_CALL *CountModFile)(const char* relative);
    /* Enabled mods in load order. */
    int32_t    (SWSE_CALL *ModCount)(void);
    SWSEStatus (SWSE_CALL *ModAt)(int32_t index, char* name, int32_t nameLen,
                                  char* path, int32_t pathLen);
    /* <game>\SWSEMods */
    SWSEStatus (SWSE_CALL *ModsRoot)(char* out, int32_t outLen);
    /* <game>\bin, where swse_log.txt and SWSE's dump files go. */
    SWSEStatus (SWSE_CALL *GameBinDir)(char* out, int32_t outLen);
    /* The mod folder your DLL was loaded from (SWSEMods\<Mod>): keep your
       settings files here, so they travel with the mod. [any thread] */
    SWSEStatus (SWSE_CALL *SelfModDir)(SWSEPluginHandle self, char* out, int32_t outLen);
    /* Your DLL's full path. [any thread] */
    SWSEStatus (SWSE_CALL *SelfPath)(SWSEPluginHandle self, char* out, int32_t outLen);
} SWSEModsAPI;

/* ==== the game: level, player, objects ==================================== */

typedef struct SWSELevelInfo {
    uint32_t size;
    uint32_t up;             /* 1 while a player body exists                     */
    uint32_t epoch;          /* 0 = no level seen yet                            */
    uint32_t ageMs;          /* how long this epoch's body has existed           */
    char     name[64];       /* "lm_level_03", or "" when SWSE does not know     */
} SWSELevelInfo;

typedef struct SWSEGameAPI {
    uint32_t size;
    uint32_t version;        /* 1 */
    SWSEStatus (SWSE_CALL *Level)(SWSELevelInfo* out);
    /* 1 exactly once per epoch: the first call after the level has been up
       settleMs. *lastEpoch is your memory of the epoch you last acted on
       (start it at 0). The per-level "apply my settings" helper. */
    int32_t    (SWSE_CALL *LevelDue)(uint32_t* lastEpoch, uint32_t settleMs);
    /* World position (Z is up). SWSE_E_NOTREADY at the menu. In safe mode
       (an unknown game build) this and the four player calls below answer
       SWSE_E_UNSUPPORTED, and Level() never reports a level up. */
    SWSEStatus (SWSE_CALL *PlayerPos)(float xyz[3]);
    /* Facing in degrees 0..360, the angle the game's level records use. */
    SWSEStatus (SWSE_CALL *PlayerYaw)(float* deg);
    SWSEStatus (SWSE_CALL *SetPlayerYaw)(float deg);
    SWSEStatus (SWSE_CALL *PlayerHealth)(float* cur, float* max);
    SWSEStatus (SWSE_CALL *PlayerStamina)(float* cur, float* max);
    /* A named position from any mod's positions.txt / sites.txt;
       SWSE_E_NOTFOUND for an unknown label. yawDeg may be NULL; a label saved
       without a facing leaves *yawDeg untouched and still returns SWSE_OK. */
    SWSEStatus (SWSE_CALL *PositionGet)(const char* label, float xyz[3], float* yawDeg);
    /* The game's own path hash of "/data/prefs/..." style paths. 0 on failure,
       and in safe mode. */
    uint32_t   (SWSE_CALL *HashPath)(const char* path);
    /* The loaded prefs object whose path hash is pathHash, or 0. Read-only
       lookup in the game's resource registry: no game call, nothing created.
       0 in safe mode (the registry is found at a Steam address). */
    uint32_t   (SWSE_CALL *ResourceLookup)(uint32_t pathHash);
    /* Is obj an instance of RTTI class rttiClass ("NPCPrefs"), directly or by
       inheritance? 1 yes, 0 no or unreadable. Never faults. */
    int32_t    (SWSE_CALL *IsA)(uint32_t obj, const char* rttiClass);
    /* The RTTI class name of a live object. SWSE_E_NOTFOUND if it has none. */
    SWSEStatus (SWSE_CALL *ClassName)(uint32_t obj, char* out, int32_t outLen);
    /* 1 when the game window really has keyboard focus (what key binds use). */
    int32_t    (SWSE_CALL *InputFocused)(void);
} SWSEGameAPI;

/* ==== memory ============================================================== */

typedef struct SWSEGameInfo {
    uint32_t size;
    uint32_t imageBase;      /* where stranger.exe is loaded                      */
    uint32_t imageSize;
    uint32_t timeDateStamp;  /* PE header stamp: identifies the exact game build.
                                RVAs in SWSE's research notes are for the Steam
                                HD build; refuse to patch on a stamp you do not
                                know.                                          */
    char     exePath[260];
} SWSEGameInfo;

typedef struct SWSEMemoryAPI {
    uint32_t size;
    uint32_t version;        /* 1 */
    /* [any thread] Copy len bytes from addr. SWSE_E_FAULT if any byte is
       unreadable (out may then be partly written). The SEH lives on SWSE's
       side, so this works from compilers without __try (MinGW...). */
    SWSEStatus (SWSE_CALL *Read)(uint32_t addr, void* out, uint32_t len);
    /* Write len bytes, lifting page protection for the write if needed.
       Threads: from any thread only for an aligned write of 4 bytes or fewer
       to memory that is already writable - x86 cannot tear such a store,
       which is what SWSE's own worker-thread tuning relies on. Anything
       larger, or anything that needs page protection lifted (which is
       process-wide while it lasts), is render-thread only and returns
       SWSE_E_THREAD elsewhere (PLUGIN_QA.md D3).
       Game data only: a range SWSE has patched (the console's `hooks` lists
       them) is refused with SWSE_E_TAKEN. A raw write bypasses the prefs
       editor, so `prefs restore` will not undo it. SWSE_E_UNSUPPORTED in
       safe mode (an unknown game build): SWSE cannot tell there which
       addresses are right. Read keeps working. */
    SWSEStatus (SWSE_CALL *Write)(uint32_t addr, const void* in, uint32_t len);
    /* [any thread] 1 if the whole range is committed and readable. Never faults. */
    int32_t    (SWSE_CALL *IsReadable)(uint32_t addr, uint32_t len);
    /* [any thread] */
    SWSEStatus (SWSE_CALL *GameInfo)(SWSEGameInfo* out);
} SWSEMemoryAPI;

/* ==== OpenGL fan-out ====================================================== */

/* SWSE owns the GL hooks and fans them out, because two hooks on one GL
   function do not coexist here: SWSE's unhook-call-rehook hooks restore the
   original bytes on every call, which silently erases any patch laid over
   them. Do not patch GL functions yourself - ask for the notification. */
typedef void (SWSE_CALL *SWSEBindTextureFn)(uint32_t target, uint32_t texId, void* user);

typedef struct SWSEGLAPI {
    uint32_t size;
    uint32_t version;        /* 1 */
    /* The real wglGetProcAddress, then opengl32's own exports. NULL if unknown. */
    void*      (SWSE_CALL *GetProc)(const char* name);
    /* Called inside glBindTexture, BEFORE the real bind, for every bind the
       game makes - thousands a frame, so do almost nothing. The first listener
       installs SWSE's glBindTexture hook (it is never removed once in; with no
       listener it costs one test). Binds you make from inside the listener are
       not reported back to you. Only while your switch is on. Inside a
       listener, Execute and (un)registering return SWSE_E_BUSY. */
    SWSEStatus (SWSE_CALL *AddBindTextureListener)(SWSEPluginHandle self, SWSEBindTextureFn fn, void* user);
    SWSEStatus (SWSE_CALL *RemoveBindTextureListener)(SWSEPluginHandle self, SWSEBindTextureFn fn, void* user);
    /* The last texture bound to GL_TEXTURE_2D as the hook saw it; 0 before
       the hook exists. */
    uint32_t   (SWSE_CALL *BoundTexture2D)(void);
} SWSEGLAPI;

/* ==== reports: selftest / perf / status lines ============================= */

#define SWSE_REPORT_SELFTEST  1u   /* `selftest`, and its automatic run per level */
#define SWSE_REPORT_PERF      2u   /* `perf`                                      */
#define SWSE_REPORT_STATUS    3u   /* `status`                                    */

#define SWSE_CHECK_FAIL   0
#define SWSE_CHECK_PASS   1
#define SWSE_CHECK_WARN (-1)       /* not applicable here - never "working"       */

/* `sink` is only valid during the callback; hand it to Line/Check. */
typedef void (SWSE_CALL *SWSEReportFn)(uint32_t kind, const void* sink, void* user);

typedef struct SWSEReportsAPI {
    uint32_t size;
    uint32_t version;        /* 1 */
    SWSEStatus (SWSE_CALL *Register)(SWSEPluginHandle self, uint32_t kind, SWSEReportFn fn, void* user);
    /* PERF / STATUS: one line of output. */
    void (SWSE_CALL *Line)(const void* sink, const char* text);
    /* SELFTEST: one "[PASS] name  detail" line, counted in the summary.
       Assert on evidence of work done (things found, polls taken, programs
       injected), never on a flag being set - SWSE's own self-test exists
       because a feature once reported ON while doing nothing. */
    void (SWSE_CALL *Check)(const void* sink, int32_t result, const char* name, const char* detail);
} SWSEReportsAPI;

/* ==== phase 4 - proposed, for review only ================================= */
#ifdef SWSE_PLUGIN_API_PROPOSED

/* The live prefs editor (`prefs`): every tunable number the game has.
   target: "/data/prefs/weapons/x.txt" | "7AE0C662" | "@RttiClass" | "&addr".
   field:  reflected name "m_health" | "Class::m_x" | "0x448", ":i :b :h :f"
   suffix. Originals are kept, so `prefs restore` undoes every write. A write
   is a heap pass that can cost a second on the render thread: batch with
   SetMany, never call per frame. Return: objects written, or SWSE_E_*. */
typedef struct SWSEPrefsAPI {
    uint32_t size;
    uint32_t version;
    int32_t (SWSE_CALL *Set)(const char* target, const char* field, const char* value);
    int32_t (SWSE_CALL *SetMany)(const char* const* targets, int32_t n,
                                 const char* field, const char* value);
    int32_t (SWSE_CALL *GetFloatMany)(const char* const* targets, int32_t n,
                                      const char* field, float* out, int32_t* found);
    int32_t (SWSE_CALL *RestoreAll)(void);
} SWSEPrefsAPI;

/* The game's own .foo script functions (`list` shows the 181 callable now).
   At most 2 arguments; no return value yet. Also reachable, untyped, through
   SWSEConsoleAPI.Execute("<FunctionName> args"). */
typedef struct SWSEScriptAPI {
    uint32_t size;
    uint32_t version;
    /* SWSE_OK, SWSE_E_NOTREADY (no script context), SWSE_E_NOTFOUND, SWSE_E_FAULT */
    SWSEStatus (SWSE_CALL *Call)(const char* function, int32_t argc, const char* const* args);
    /* "" / "i" / "if" / "e"...: i int, f float, b bool, e enum. */
    SWSEStatus (SWSE_CALL *ArgFormat)(const char* function, char* out, int32_t outLen);
    int32_t    (SWSE_CALL *HaveContext)(void);
} SWSEScriptAPI;

/* Verified inline hooks at function entry, through SWSE so that every
   patched address has one registered owner. SWSE refuses (SWSE_E_TAKEN) any
   range it or another plugin already patched, refuses when the bytes at the
   target are not exactly `expect`, and refuses relative branches inside the
   stolen bytes. The patch is never removed: SetEnabled flips an indirect
   jump between your detour and the original, and SWSE flips it to the
   original itself when your plugin is switched off or faults. */
typedef struct SWSEHookDesc {
    uint32_t       size;
    uint32_t       target;     /* absolute address (GameInfo imageBase + RVA)    */
    const uint8_t* expect;     /* the exact bytes at target, stealLen of them    */
    uint32_t       stealLen;   /* 5..16, ending on an instruction boundary       */
    void*          detour;     /* yours; call/jump *trampoline for the original  */
    const char*    label;      /* for `hooks`: "Bolt::Update"                    */
} SWSEHookDesc;

typedef struct SWSEHooksAPI {
    uint32_t size;
    uint32_t version;
    SWSEStatus (SWSE_CALL *Install)(SWSEPluginHandle self, const SWSEHookDesc* desc,
                                    void** trampoline, uint32_t* hookId);
    SWSEStatus (SWSE_CALL *SetEnabled)(SWSEPluginHandle self, uint32_t hookId, int32_t on);
    /* Swap one vtable slot (in .rdata; reversible, and it cannot race a thread
       mid-instruction the way an inline patch can). */
    SWSEStatus (SWSE_CALL *VtableSwap)(SWSEPluginHandle self, uint32_t vtable, uint32_t slot,
                                       void* fn, void** original, uint32_t* hookId);
    /* Who patched addr, if anyone: 1 and the owner's name, or 0. */
    int32_t    (SWSE_CALL *Owner)(uint32_t addr, char* out, int32_t outLen);
} SWSEHooksAPI;

#endif /* SWSE_PLUGIN_API_PROPOSED */

#pragma pack(pop)

/* ==== layout checks: these offsets are frozen by the ABI ================= */

#define SWSE_STATIC_ASSERT(cond, tag) typedef char swse_static_assert_##tag[(cond) ? 1 : -1]
SWSE_STATIC_ASSERT(sizeof(void*) == 4, pointers_are_32_bit);
SWSE_STATIC_ASSERT(offsetof(SWSEInterface, GetInterface) == 16, core_interface_layout);
SWSE_STATIC_ASSERT(offsetof(SWSEPluginInfo, flags) == 32, plugin_info_layout);
SWSE_STATIC_ASSERT(offsetof(SWSEConsoleAPI, Post) == 28, console_layout);
SWSE_STATIC_ASSERT(offsetof(SWSECommandDesc, flags) == 24, command_desc_layout);
SWSE_STATIC_ASSERT(offsetof(SWSEFrameInfo, consoleOpen) == 40, frame_info_layout);
SWSE_STATIC_ASSERT(offsetof(SWSELevelInfo, name) == 16, level_info_layout);

/* ==== helpers, compiled into your plugin ================================== */
#ifndef SWSE_PLUGIN_NO_HELPERS
#include <stdarg.h>
#include <stdio.h>

/* const SWSEConsoleAPI* con = SWSE_GET(swse, SWSEConsoleAPI, SWSE_IFACE_CONSOLE, 1); */
#define SWSE_GET(swse, Type, id, minVersion) \
    ((const Type*)((swse)->GetInterface((id), (minVersion))))

/* Does this table (as served by the running SWSE) have `member`? Use before
   calling a function newer than the version you asked GetInterface for. */
#define SWSE_HAS(tbl, Type, member) \
    ((tbl) != NULL && (tbl)->size >= (uint32_t)(offsetof(Type, member) + sizeof(((Type*)0)->member)))

/* printf-style Print/Write, formatted by YOUR C runtime (SWSE's own
   formatter has no %f), then handed over as a finished string. */
static SWSE_INLINE void SWSE_Printf(const SWSEConsoleAPI* con, SWSEPluginHandle self,
                                    const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    if (!con || !fmt) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    con->Print(self, buf);
}

static SWSE_INLINE void SWSE_Logf(const SWSELogAPI* log, SWSEPluginHandle self,
                                  int32_t level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    if (!log || !fmt) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    log->Write(self, level, buf);
}

/* For SWSESwitchFn: write a formatted one-liner into SWSE's msg buffer. */
static SWSE_INLINE void SWSE_Msg(char* msg, int32_t msgLen, const char* fmt, ...) {
    va_list ap;
    if (!msg || msgLen <= 0 || !fmt) return;
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)msgLen, fmt, ap);
    va_end(ap);
    msg[msgLen - 1] = 0;
}
#endif /* SWSE_PLUGIN_NO_HELPERS */

/* ==== your two exports (defined by the plugin; SWSE defines SWSE_HOST) ==== */
#ifndef SWSE_HOST
SWSE_PLUGIN_EXPORT int32_t SWSE_CALL SWSEPlugin_Query(const SWSEInterface* swse, SWSEPluginInfo* info);
SWSE_PLUGIN_EXPORT int32_t SWSE_CALL SWSEPlugin_Load(const SWSEInterface* swse);
#endif

#endif /* SWSE_PLUGIN_API_H */
