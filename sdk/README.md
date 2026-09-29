# SWSE plugin SDK

Write native plugins for SWSE, the Stranger's Wrath Script Extender, without
touching SWSE's source.

> **Plugin API version 1, served by SWSE 1.1** (in development). Everything in
> the header is served except the three tables behind
> `SWSE_PLUGIN_API_PROPOSED` (prefs, script VM, hooks), which `GetInterface`
> answers with NULL. The design, the reasoning behind every rule below and
> what is still planned are in
> [`swse/research/PLUGIN_SYSTEM.md`](../swse/research/PLUGIN_SYSTEM.md); what
> the fault guard can and cannot catch, measured, and the loader's in-game test
> results are in [`swse/research/PLUGIN_QA.md`](../swse/research/PLUGIN_QA.md).

| File | What it is |
|---|---|
| `swse_plugin_api.h` | the whole API: one C header, no library to link |
| `examples/hello_plugin/` | a complete plugin: a command, a frame counter, a level-up message |
| `tools/mockhost/` | a stand-in host for trying a plugin outside the game, a deliberately faulty test plugin, and a self-check of the loader rules |
| `tools/qaplugins/` | the test plugins behind `tools\swse_plugin_tests.ps1`, SWSE's in-game test pass of the loader; several misbehave on purpose - never ship them |

---

## Do you need a plugin?

Most mods do not. SWSE already reads plain text files from mod folders, and
text is easier to write, share and fix:

| You want to... | Use |
|---|---|
| bundle console commands under one name | `alias`, or a `scripts\<name>.txt` file |
| react to something happening in a level | `triggers.txt` (its `do =` lines are console commands) |
| change a game value on every level | `prefs.txt`, `playerprefs.txt`, `aiprefs.txt`, `characters.txt` |
| add plants to the wind, HD textures | `foliage.txt`, `textures\*.oft` |

Write a plugin when you need code: logic that runs every frame, reading game
memory, drawing on screen, reacting to texture loads or binds, or anything a
console command cannot express.

---

## Quick start

1. Install Visual Studio 2022 or later with the C++ workload - the same
   toolchain that builds SWSE.
2. Run `examples\hello_plugin\build.bat`. It produces `hello.dll`.
3. Copy it to `<game>\SWSEMods\SWSE Hello\plugins\hello.dll`.
4. Start the game, open the console with `~` and type `features hello on`.
   SWSE loads the plugin and saves the switch to `SWSEMods\features.txt`.
5. Type `hello`, or `hello Sekto`. Load a level and watch for
   `hello: a level is up`.

`features hello off` switches it off again. `plugins` lists every plugin SWSE
found, with its state and how much frame time it costs; `plugins hello`
shows one in detail.

---

## How SWSE runs a plugin

**Where it lives.** `SWSEMods\<Mod>\plugins\<name>.dll`. A mod is just a
folder, so a plugin ships next to its data files and is enabled, ordered and
disabled with its mod in `load_order.txt`. If two enabled mods ship a DLL with
the same name, the one later in load order is used and `plugins` shows the
other as shadowed.

**Its switch.** Every plugin is a feature switch named after its file:
`hello.dll` is `hello`. Like every SWSE system since 1.1, it is **off until the
user switches it on** - `features hello on` in the console, or `hello = on` in
`features.txt`. While it is off, SWSE does not even load the DLL, so a plugin
dropped into a mod folder never runs code until someone asks for it by name.
(`features all on` does not include plugins; `features all off` does.) The
name must be 1-31 characters of `a-z 0-9 _ -` and cannot be a built-in
switch's name or nickname (`graphics.dll` is refused). Nicknames you give in
`SWSEPluginInfo.aliases` work once your plugin has loaded.

**The sequence.** On the first switch-on, on the game's render thread:

| Step | You | Rules |
|---|---|---|
| `SWSEPlugin_Query(swse, info)` | fill in `SWSEPluginInfo`, return 1 | describe yourself only; `info->name` must equal the file name |
| `SWSEPlugin_Load(swse)` | fetch the tables you need, register commands, callbacks and events, return 1 | runs once per session; return 0 and SWSE rolls back everything you registered |
| your `onEnable` | start your effect | write a one-line message; return 0 to stay off |
| ... frames, events, commands ... | | only while switched on |
| your `onDisable` | undo what you changed | `features hello off` |
| your `onEnable` again | | a later `features hello on` - Load does not run again |

**Never unloaded.** Switching a plugin off stops every callback, but the DLL
stays loaded until the game exits - and so does one SWSE refused, or switched
off after a fault, once LoadLibrary had run its DllMain. Code that the game or
the driver may be executing cannot be unloaded safely - SWSE learned this the
hard way with its own hooks - so to try a rebuilt DLL, restart the game. (A
developer reload for plugins that promise not to patch anything is planned for
later.)

**Faults.** SWSE calls into your plugin inside a structured-exception guard.
If your code faults - a bad pointer, a C++ exception escaping, a stack
overflow - the game keeps running, your plugin is switched off for the rest
of the session, and the console and `bin\swse_log.txt` say where:

```
plugin 'hello' FAULTED: exception C0000005 in a command at hello.dll+0x1019 - switched off for this session
plugin 'hello' FAULTED: a C++ exception escaped a frame callback (thrown by hello.dll) - switched off for this session
plugin 'hello' FAULTED: exception C0000005 in a command at ntdll.dll+0xB54BB, called from hello.dll+0x11FC - switched off for this session
```

The last form is a fault inside something you called (the C runtime, the
game, Windows) with a bad argument: the offset is your call. Look offsets up
in your `.map` file or in the debugger. A faulted plugin stays off until the
next launch; `features.txt` is not changed, so a fixed build loads normally
next time.

**What the guard cannot catch.** The guard covers only code running inside a
call from SWSE, on the render thread. These still take the whole game down
(measured, see `swse/research/PLUGIN_QA.md`):
- **A fault on a thread you started.** Wrap your thread procedure in your own
  `__try`/`__except` (or `try`/`catch`), and report failures through
  `SWSELogAPI.Write`.
- **Heap corruption.** Writing past the end of a block returns normally and
  crashes the game later, somewhere else. Build with `/RTC` and page heap
  while developing.
- **Code the game calls directly**, such as a function you patched yourself.
- `abort()`, `exit()`, `TerminateProcess`, and hangs.

---

## The rules

These are what keep the game running with several plugins from different
people in it.

**ABI.**
- 32-bit x86 DLL (`vcvars32.bat`); the header refuses to compile otherwise.
- Build with the static C runtime (`/MT`), so there is no redistributable to
  install and no C runtime state shared with SWSE.
- Everything crosses the boundary as plain C: `const char*`, numbers, structs
  with a leading `size`. Never pass memory for the other side to free, never a
  `FILE*`, never a C++ object, never let an exception out of a callback.
- Strings you pass are copied by SWSE before the call returns. Strings you get
  back are written into your buffer and always NUL-terminated.
- Set `size = sizeof(...)` on every struct you hand to SWSE.

**Threads.** Everything happens on the game's render thread, the one that
presents frames, with the game's GL context current. Every API function is
render-thread-only unless the header marks it `[any thread]`; called from
another thread it returns `SWSE_E_THREAD` and does nothing. If you start a
worker thread, bring results back with `SWSEConsoleAPI.Post` (it runs the line
at the top of the next frame, in order, while your plugin is on) or your own
queue drained in a frame callback, and stop the thread in `onDisable`.

**The frame is shared.** Your callbacks run inside the game's frame. SWSE
times every callback; `plugins` shows the cost, and a frame stall in
`swse_log.txt` names the plugin that caused it. Do not block, do not sleep,
and spread expensive work over frames. Console commands that scan the heap
(`prefs`, `knockback`, `weapons`, `npcguns`) can take a second: never run them
every frame.

**GL state.** Restore every GL binding and enable you change. In the OVERLAY
phase SWSE saves and restores the attribute stacks, the matrices, the GLSL and
ARB program bindings, the framebuffer and the vertex/element buffers around all
plugins - a stack you leave pushed is popped, and after a fault inside
`glBegin` SWSE closes the primitive - so the console, drawn next, is safe; in
TICK, and in GL listeners, you are on your own. Never patch a GL function
yourself: SWSE's GL hooks put the original bytes back on every call and would
silently erase yours. Ask for the notification instead (`SWSEGLAPI`, and the
`SWSE_EV_TEXTURE_UPLOAD` event). Inside a bind listener or the upload event,
`Execute` and (un)registering answer `SWSE_E_BUSY`: you are inside the game's
GL call. Queue the work (`Post`, or a flag your TICK reads) instead.

**DllMain.** Do nothing in `DllMain` beyond what the C runtime does; it runs
under the Windows loader lock. Start work in `Load` and `onEnable`.

**Off means off.** In `onDisable`, put back what you changed - values, GL
programs, patched memory - so the game runs as if you were not there. That is
the promise every SWSE switch makes.

---

## What you can use

`swse->GetInterface(id, minVersion)` returns a table of functions, or NULL if
the running SWSE does not have it. Cache the pointers. `SWSE_GET` wraps the
cast; `SWSE_HAS(table, Type, Function)` tells you whether a function newer
than the version you asked for exists.

| Table | Id | What | Available |
|---|---|---|---|
| `SWSELogAPI` | `SWSE_IFACE_LOG` | lines in `bin\swse_log.txt`, tagged with your name | 1.1 |
| `SWSEConsoleAPI` | `SWSE_IFACE_CONSOLE` | print; add commands; run any console line; `Post` from any thread (version 2) | 1.1 |
| `SWSEFrameAPI` | `SWSE_IFACE_FRAME` | TICK and OVERLAY callbacks every frame | 1.1 |
| `SWSEEventsAPI` | `SWSE_IFACE_EVENTS` | level up / down, `mods reload`, a switch changed, texture uploads | 1.1 |
| `SWSEFeaturesAPI` | `SWSE_IFACE_FEATURES` | your on/off handlers; is another switch on? | 1.1 |
| `SWSEModsAPI` | `SWSE_IFACE_MODS` | mod files (additive / last-wins), your own mod folder | 1.1 |
| `SWSEGameAPI` | `SWSE_IFACE_GAME` | level state, player position / facing / health, named positions, path hash, prefs lookup, RTTI | 1.1 |
| `SWSEMemoryAPI` | `SWSE_IFACE_MEMORY` | fault-safe read and write of game memory (bytes SWSE has patched are refused; no write in safe mode); game build info | 1.1 |
| `SWSEGLAPI` | `SWSE_IFACE_GL` | glBindTexture listener, GL entry points, the bound texture | 1.1 |
| `SWSEReportsAPI` | `SWSE_IFACE_REPORTS` | your lines in `selftest`, `perf`, `status` | 1.1 |
| prefs, script VM, hooks | | proposed; in the header behind `SWSE_PLUGIN_API_PROPOSED` | later |

**Every console command is already an API.** `Execute(self, line)` runs a
line exactly as if it were typed, so all of SWSE's 200-odd commands and the
game's own script functions are yours from day one:
`"grant surgerybid 1"`, `"warp 3"`, `"prefs set @NPCPrefs m_health 500"`,
`"spawnat enemyambush1 3"`. The typed tables exist for what a command cannot
do: return values, per-frame reads, callbacks.

---

## Recipes

**A command.**

```c
static void SWSE_CALL Cmd_Heal(int32_t argc, const char* const* argv, void* user) {
    g_con->Execute(g_self, "heal");                 /* reuse the built-in */
    SWSE_Printf(g_con, g_self, "patched up");
}
...
SWSECommandDesc d = { 0 };
d.size = sizeof(d); d.name = "patchup"; d.help = "patchup - heal and say so"; d.fn = Cmd_Heal;
if (g_con->RegisterCommand(g_self, &d) == SWSE_E_TAKEN) { /* pick another name */ }
```

Names are 1-31 characters of `a-z 0-9 _ - .`. SWSE's built-in commands can
never be replaced: registering one of their names returns `SWSE_E_TAKEN`. Two
plugins may offer the same name; both registrations stand, and while both are
on, the plugin **later in load order** runs it - the same "later mods win
conflicts" rule as `load_order.txt` - so the result never depends on which
plugin was switched on first. Switch the later one off and the other one's
command answers again. `help <name>` lists every provider. Your commands
appear in `help` under your plugin's name, complete with Tab, can be bound to
keys (`bind F6 patchup`), used in aliases and in a trigger's `do =` line, and
work from the remote mailbox: everything you `Print` while one runs from the
mailbox lands in `remote_out.txt`. For tools, print one line of `key=value`
pairs, as SWSE's own `query` does.

**Apply settings once per level.**

```c
static uint32_t g_lastEpoch;
static void SWSE_CALL OnTick(const SWSEFrameInfo* f, void* u) {
    if (g_game->LevelDue(&g_lastEpoch, 4000))       /* 4 s after the player appears */
        ApplyMySettings();                          /* once per level */
}
```

`SWSE_EV_LEVEL_UP` fires the moment a player body appears, before the level's
cast has finished spawning; `LevelDue` waits for it to settle, which is what
SWSE's own per-level systems do.

**A settings file that travels with your mod.**

```c
char dir[260], path[300];
g_mods->SelfModDir(g_self, dir, sizeof(dir));       /* <game>\SWSEMods\My Mod */
snprintf(path, sizeof(path), "%s\\mysettings.txt", dir);
```

Reading a conventional file that other mods may also provide? Use
`ForEachModFile` for lists every mod adds to, `FindModFile` for a single
value where the last mod in load order wins - the same two rules SWSE's own
files follow.

**Read game memory without crashing.**

```c
float hp;
if (g_mem->Read(npc + 0x78, &hp, sizeof(hp)) == SWSE_OK) { ... }
```

The fault guard is on SWSE's side, so this works from any compiler. Offsets
come from `swse/research/`; they are for the Steam HD build - check
`GameInfo().timeDateStamp` before writing anywhere.

**Safe mode.** On a game build SWSE does not recognise (GOG, a future patch),
SWSE runs in safe mode, and your plugin still loads. The calls that would
reach the game answer `SWSE_E_UNSUPPORTED` - `Game.PlayerPos`, `PlayerYaw`,
`SetPlayerYaw`, `PlayerHealth`, `PlayerStamina`, `Memory.Write` - or 0
(`Game.HashPath`, `ResourceLookup`); no level event arrives; and `Execute` of
a command that reaches the game prints the refusal and does nothing. Everything
else works. Treat `SWSE_E_UNSUPPORTED` as final: unlike `SWSE_E_NOTREADY`, it
does not change later in the session.

**Draw something.** Register an `SWSE_FRAME_OVERLAY` callback: it runs after
SWSE's post-process and before the console, so the console stays on top.
`frame->width/height` is the window's client area.

---

## Debugging

- Try it outside the game first: `tools\mockhost\build.bat` builds a stand-in
  host that serves the MVP tables with SWSE's dispatch rules, and runs a
  self-check. Then `mockhost.exe myplugin.dll -- "features myplugin on"
  "frame 3" "levelup" "mycommand arg"` runs your plugin's Query, Load,
  switch, callbacks and commands and prints what it would print in the
  console. It has no game: level and player queries answer "not ready".
- `bin\swse_log.txt` holds your `Log` lines as `[yourname] ...`, SWSE's load
  messages for your plugin (the path and SHA-256 it loaded, why it was
  refused, if it was), and any fault.
- `plugins` in the console: state, version, frame cost, last fault;
  `plugins <yourname>` adds the file, the full SHA-256 and everything you
  registered. `query plugins` gives each plugin's state as `key=value` for
  tools. `perf` shows your frame cost, and a `FRAMESTALL` line in the log names
  your plugin when it made a frame slow.
- `hooks` lists every code and table patch SWSE has live: if your plugin
  patches something anyway, check it is not on that list.
- Build with `/Zi` and link with `/DEBUG` to get a `.pdb`, then attach Visual
  Studio to `stranger.exe` (Debug > Attach to Process). Breakpoints in your
  plugin work once it is loaded.
- Keep a `.map` file (`/link /MAP`) for each build you hand out, so a fault
  offset from a user's log can be turned into a function name.
- The game has to be started through `Launcher.exe`; SWSE's `exit` command
  quits it cleanly when you need to replace your DLL.

---

## Compatibility

What you can rely on:

- Plugin API version 1 is served by every SWSE that implements plugins,
  until a version 2 is announced; no breaking change is planned. If one ever
  happens, SWSE keeps serving version 1 alongside it for at least one release.
- A table only grows. Existing functions keep their place, types and meaning;
  new ones are appended and raise the table's `version`. A function SWSE can
  no longer honour stays in the table and returns `SWSE_E_UNSUPPORTED`.
- Event ids, interface ids, flags and status codes are never reused.
- Your plugin runs on any SWSE at or above your `minSwseVersion` that serves
  API version 1. An older SWSE refuses it with a message instead of crashing.

What you cannot rely on: the order in which two plugins' callbacks run (it
follows load order today, but that is not a promise), and anything the header
does not say.

---

## Distributing a plugin

```
My Mod\
  mod.json              name, author, version, description (for the Mod Loader)
  plugins\mymod.dll     the plugin; its switch is `mymod`
  mymod.txt             your settings, found with SelfModDir
  README.txt            what it does, and "type `features mymod on`"
```

- Ship a release build with the static runtime (`/MT`), not a debug build.
- Give the DLL a version resource (see `examples/hello_plugin/hello.rc`):
  Windows and SWSE can read it without running your code.
- A plugin is native code with the same rights as the game. Say what it does,
  ship the source if you can, and tell users to install plugins only from
  people they trust.

---

## FAQ

**Can my plugin hook a game function?** Not through SWSE yet. A verified,
registered hook helper is designed (`SWSEHooksAPI` in the header, behind
`SWSE_PLUGIN_API_PROPOSED`). Until then do not patch code: SWSE patches
several game and GL functions - `hooks` in the console lists the ones live
right now, and `PLUGIN_SYSTEM.md` appendix A all of them - and two patches on
one function break each other. `Memory.Write` refuses the bytes on that list.

**C++, the STL, my own libraries?** Yes, inside your DLL. Only the boundary
has to be C. Put dependency DLLs next to your plugin; SWSE loads plugins so
that their own folder is searched first.

**Other compilers?** The ABI is plain C with `__cdecl`, so MinGW, clang-cl,
Rust or Zig should all work, but only MSVC is tested. `SWSEMemoryAPI` gives
fault-safe memory access to compilers without `__try`.

**64-bit?** No. The game is a 32-bit process.

**Why can't I unload and rebuild while the game runs?** Windows locks a
loaded DLL, and a DLL whose code the game may still be executing cannot be
unloaded safely. Restart the game; SWSE's `exit` command makes it quick.
