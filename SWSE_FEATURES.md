# SWSE - Stranger's Wrath Script Extender

Everything SWSE 1.1.1 can do. The SWSE DLL is the runtime half of the project:
a `dinput8.dll` proxy loaded by the game, giving an in-game console, graphics
features, gameplay tuning from text files, and the reverse-engineering
instruments the rest of the project is built with. The offline half (archive
parsing, texture pipeline, the Mod Loader GUI) is the `oddforge` Python library
and `studio.py`.

**Install:** `swse/build.bat`, then copy `dinput8.dll` into the game's `bin\`.
A `dinput8_real.dll` beside it is optional: since 1.0.1 SWSE loads the real
system `dinput8.dll` by absolute path when that file is absent. The DLL carries
a version resource (1.1.1.0 - Properties > Details); in game, `ver` and
`query version` say the same.

What changed release by release is in [CHANGELOG.md](CHANGELOG.md). The guide
for tools built on SWSE - Stranger: Armed to the Teeth in particular - is
[AT3_INTEGRATION.md](AT3_INTEGRATION.md), and what a tool can rely on from
release to release is [TOOL_CONTRACT.md](TOOL_CONTRACT.md).

**Paths on this page.** `swse/research/...` and `tools/...` name files in the
project's development repository. Of the research notes, the public
repository carries only `swse/research/REFLECTION_SCHEMA.md`, and none of the
`tools/` scripts; where this page names one, it says where the work is
recorded, not a file you will find here.

---

## What is new in 1.1.1

* **The console's new look and keys** - the frame rate (fps and ms) at the top
  right, sharper coloured text, suggestions as you type, history on Up/Down,
  and copy and paste (Ctrl+V / Shift+Insert, Ctrl+C, Ctrl+Shift+C). See
  [The console's look and keys](#the-consoles-look-and-keys-111). No new
  commands; nothing else changed from 1.1.

## What is new in 1.1

* **Console-only by default.** A fresh install switches on the console and
  nothing else, so the game plays exactly as shipped until you choose
  otherwise. Nothing was removed: every system comes back with one line in
  `features.txt`, or live with `features <name> on`.
* **Systems switch on and off while the game runs** (`features`), and the
  choice is saved unless you add `temp`.
* **Play as another character** (`playnpc`): become any character in the
  level, with its own body and animations. One that is not in memory is
  streamed in first, and it stays loaded while you play it. See
  [Play as a character](#play-as-a-character---playnpc-11).
* **A free-flying view** (`freecam`): the game's own developer fly camera,
  switched on from the console. Stranger stays put while you fly, and `pos`,
  `look` and `speed` drive it with no real input. See
  [Free camera](#free-camera---freecam-11).
* **Player tuning** (`playerprefs.txt`, `playertune`): health, stamina, run
  speed, jump, gravity and air control, every level.
* **A live prefs editor** (`prefs`, `prefs.txt`, `knockback`): read and write
  any loaded prefs record by path, hash or class, found through the game's own
  resource registry, with field names from an exact table of the game's 5,697 reflected
  fields. Nothing on disk changes.
* **Facing is saved with every position** - `savepos`/`tp`, `writepos`/`goto`,
  `positions.txt`, and a new `sites.txt` in the column order AT3 reads - as the
  same angle the game's level records use.
* **Automation:** `;` sequences, `wait`, `after`, `repeat`, `exec`, key
  binds and aliases.
* **The console keeps what you type.** While it is open, the game gets no
  keyboard or mouse input: typing never walks Stranger, and the mouse does not
  turn the camera.
* **For tools:** `status`, `query` (key=value), `mods`, `hide`/`show`, `log`.
* **misstime** in `aiprefs.txt` is now applied (in milliseconds, 10 = vanilla).
* A **level watcher** replaces the hit-reaction actor list as the "a level is
  up" signal, which fixes AI tuning, triggers and the self-test with hit
  reactions off.
* **Background mode for tools** (`SWSE_AGENTDEBUG=1`): the game runs muted
  behind other windows, cannot take the keyboard from the app you are in, and
  takes commands through the mailbox - see [AgentDebugMode](#agentdebugmode).
  `mute` switches the sound.
* **A game-build check, and a safe mode.** On a build SWSE's addresses were
  not measured on (GOG, a future patch), nothing that patches, calls or reads
  the game runs, so SWSE cannot corrupt it; the console, graphics, HD
  textures, foliage and ray tracing still work - see
  [Game builds and safe mode](#game-builds-and-safe-mode-11).
* **Ray tracing, as an experiment.** Ray-traced ambient occlusion is an
  eleventh switch, `raytrace`, off by
  default and only with `graphics` on - see
  [Ray tracing (experimental)](#ray-tracing-experimental). Its occlusion is
  known to be wrong.
* **Menus without keys.** `menu continue` loads the last save, `menu skip`
  skips a paused movie, and `menu fs` sends any Flash screen's own command,
  the way its button does - so they work with the game behind other windows.
* **A tool contract** ([TOOL_CONTRACT.md](TOOL_CONTRACT.md), `query
  contract`): the mailbox, `query` keys, switches and files that hold for
  every 1.x release.
* **The log keeps itself tidy.** `bin\swse_log.txt` starts over past 4 MB (the
  previous one is kept as `swse_log.old.txt`), and every session opens with a
  line naming the SWSE version, the time and the DLL's build stamp.
* **Native plugins.** A mod can ship code: a DLL in its `plugins\` folder that
  SWSE calls through a small C API (`sdk\`). Each plugin is a switch named
  after its DLL, off - not even loaded - until you name it with
  `features <name> on`. A plugin that crashes is switched off and named; the
  game keeps running. See [Native plugins](#native-plugins-11) and
  [sdk/README.md](sdk/README.md).
* **`hooks`** lists every code or table patch SWSE has live, and the system
  that owns it.
* Latent 1.0.x bugs fixed - some only showed once systems could be off, the
  rest came out of an exact table of the game's reflected fields - see
  [CHANGELOG.md](CHANGELOG.md).

220 console commands in all (1.0.2 had 179), not counting plugins' own.

---

## Feature switches

SWSE is several independent systems in one DLL. `SWSEMods\features.txt` has
one line per system:

```
console    = on     # in-game console (~ key) + remote mailbox
graphics   = off    # post-process look (F10 once on)
hdtextures = off    # HD texture pack (needs the 4GB patch)
hitreact   = off    # NPCs flinch where they are shot
foliage    = off    # grass and plants sway in the wind
aituning   = auto   # enemy AI profiles from aiprefs.txt
triggers   = off    # mod-defined events (ambushes)
npctuning  = off    # per-character health / gib rules
playertune = auto   # your health, stamina, speed, jump...
prefsedit  = off    # live edits to the game's own values
raytrace   = off    # ray-traced AO, EXPERIMENTAL (needs graphics)
```

The shipped file is a switchboard: three lines of header, then these eleven
lines, then the long explanation of each switch as a reference section. Only
the switch lines count; the `features` command keeps every comment, a switch
line's trailing note included, when it saves.

**Three easy ways to switch.**
- **SWSE Setup** (`SWSE Setup.exe` in the download; the modders' Mod Loader
  shows it as its first tab): a tick box per switch, the presets below as
  buttons, and Install / Update, Apply and Uninstall. With the game running,
  Apply switches live through the mailbox. See
  [README.md](README.md#swse-setup-the-players-installer).
- **A preset** in the console: `features preset full` gives the classic SWSE
  in one word.
- **The file**: change `off` to `on`, save, start the game.

**Only the console is on unless the file says otherwise.** A missing file, or
a missing line, means the default: the console on, `aituning` and `playertune`
auto (below), every other system OFF. 1.0.x defaulted everything ON, and a
clean install now leaves the game exactly as shipped apart from the console.
`on`, `true`, `yes`, `1` and `enabled` count as on; anything else is off.

**Off at launch means no hooks at all.** A disabled system is not a hook that
runs and returns early - it is never installed, so the game runs as if SWSE had
never touched that system. That is the right first stop when debugging:
turning everything off except the system you are working on removes every
other variable.

### Switching live - `features`

| Command | What it does |
|---|---|
| `features` | every switch, on or off, with what it controls |
| `features <name> on\|off` | switch now **and** save to `features.txt` |
| `features <name> on\|off temp` | switch now, this session only |
| `features aituning\|playertune auto` | follow the system's own file (below), and save that |
| `features all on\|off` | every system except the console. `all on` never switches a plugin on (a plugin runs only when named); `all off` switches plugins off too |
| `features preset full [temp]` | the classic SWSE, the 1.0.x look: console, graphics, hdtextures, hitreact, foliage and aituning on; every other switch as it is |
| `features preset default [temp]` | the 1.1 default: console on, aituning and playertune auto, everything else off (plugins too) |
| `features preset list` | what each preset sets |
| `features <plugin> on\|off` | a native plugin's switch (1.1): the first `on` loads its DLL - see [Native plugins](#native-plugins-11) |

A preset saves like `features` (or not, with `temp`), leaves the switches safe
mode refuses as they are and names them, and says which of the switches it
turned on take full effect only from the next level load (`hdtextures`,
`hitreact`, `foliage`).

The console command accepts nicknames as well as the names above: `grass`,
`wind`, `plants` (foliage), `hd`, `textures` (hdtextures), `gfx`, `postfx`,
`rtgi` (graphics), `ai`, `difficulty` (aituning), `ambushes` (triggers),
`characters`, `tuning` (npctuning), `player`, `playerprefs` (playertune),
`prefs` (prefsedit), `combat`, `hitreactions` (hitreact), `rt`, `rtao`,
`raytracing` (raytrace). `features.txt` itself only understands the
canonical names.

**Auto (aituning, playertune).** These two switches also take `auto`, and ship
that way. Each runs only while its own file asks for something - an `active =
<profile>` line in `aiprefs.txt`, or any value in `playerprefs.txt` - and stays
off otherwise. Auto is decided at launch; `features <name> auto` re-checks the
file now. An explicit `on` or `off` always wins. `features` shows an auto switch
as `auto:on` or `auto:off`.

**Saving.**
- Only each switch line's value is rewritten. Its indentation and any trailing
  comment are kept, as is every other line.
- The new file is written beside the old one, then swapped in, so a crash
  cannot leave a half-written file.
- A switch that is already in the state you ask for is still saved when the
  file says otherwise. For example, `features console on` after `features
  console off` answers "already on - saved that to features.txt", and
  `features foliage on` after `features foliage on temp` makes it permanent.

Switching **on** installs a system's hooks at that moment. Switching **off**
stops it and puts shipped values back where the system keeps them; a hook it
already installed stays in place, inert, until the next launch. Per system:

| Switch | Switched on live | Switched off live |
|---|---|---|
| `console` | always on | refused - the console cannot remove itself; `features console off` only writes `off` to the file for the next launch (and then nothing is left to switch it back on with); with `temp` it is refused outright |
| `graphics` | loads the pipeline and the FBO / texture-upload hook; the **look starts off** - F10 or `gfx on` shows it | effect off, the game renders as shipped |
| `hdtextures` | indexes every mod's `textures\` folder; textures uploaded from then on are replaced (warp or load a save for a whole level) | textures uploaded from then on are vanilla |
| `hitreact` | reads `hitreact.txt`; with `enabled 1` installs the pose hook, bolt tracking and the damage watch. It has to find the level's characters first, so it is fully in effect from the next level load | reactions stop; the pose hook is left in place, inert (unpatching it is not thread-safe) |
| `foliage` | installs the texture-upload hook, reads `foliage.txt` and `wind.txt`. Plants are recognised as a level uploads them, so it starts with the next level | wind off (original vertex programs restored at once), bind tracking off |
| `aituning` | reads `aiprefs.txt`; the `active` profile is applied at once if the level has been up 4 s | every tuned object back to its shipped values |
| `triggers` | reads every `triggers.txt` and arms them; a level that has been up 4 s counts as already loaded, so its `levelload` triggers do not fire at the switch | triggers stop firing |
| `npctuning` | reads `console.txt` and every `characters.txt`, arms the spawn hook; applies as the next level spawns | stops applying; characters already tuned keep it until the level reloads |
| `playertune` | reads `playerprefs.txt`, applies at once if a level is up | the game's own values put back |
| `prefsedit` | reads every `prefs.txt`, applies at once if the level has been up 4.5 s | every edited value back to shipped |
| `raytrace` | refused unless `graphics` is on and its pipeline loaded; the tracer then follows `rtao_enable` in `graphics.txt` | the pipeline renders without it from the next frame |

A switch can refuse to come on: `raytrace` without a running graphics
pipeline, and, in safe mode (a game build SWSE's addresses were not measured
on - see [Game builds and safe mode](#game-builds-and-safe-mode-11)),
`hitreact`, `npctuning`, `aituning`, `triggers`, `playertune` and
`prefsedit`, which act on the game. A refused switch reads off for the
session - at launch as well as live - the answer says `refused`, and
`swse_log.txt` says why; `features.txt` keeps its line. `features graphics
off` takes `raytrace` off with it, the same way.

The self-test lists the systems that are switched off on one `[OFF ]` line,
never as a failure.

---

## Game builds and safe mode (1.1)

Every address SWSE uses inside `stranger.exe` - its hooks, the game functions
it calls, the objects it reads - was measured on one build: the Steam release
of Stranger's Wrath HD. At the first frame SWSE identifies the running exe
from its PE header (link time, image size, entry point), and the log names
it; `status` shows it when it is not Steam HD, and `query version` answers
`build=Steam_HD` or `build=unknown`.

On any other build (the GOG release, a future patch) SWSE runs in **safe
mode**: nothing that patches game code, calls a game function or reads the
game through one of those addresses runs. Writing into the game's code at the
wrong place is the likely cause of the reports that the console crashes the
GOG version. Safe mode has not been tested on a GOG exe yet.

**What works in safe mode**
- The console and the mailbox, and the commands that use only SWSE itself:
  `help`, `status`, `query`, `ver`, `features`, `mods`, `plugins`, `hooks`,
  `selftest`, binds and aliases, `exec`, `after`, `repeat`, `wait`,
  `scripts`, `log`, `echo`, `snap`, `mute`, `agentdebug`, and the key presses
  (`key`, `newgame`, `continue`, `skipcut`, `menu <n>`).
- The OpenGL-side switches and their commands: `graphics` (with `gfx` and
  `set <graphics key>`), `hdtextures`, `foliage` and `raytrace`. The plants
  sway, but do not part around the player: the push needs the player's
  position.
- The memory readers that read only an address you type, never a Steam one:
  `peek`, `dumpaddr`, `whatis`, `diff`, `instances`, `vtscan`, `findval`,
  `narrow` - what porting SWSE to another build needs.
- Native plugins load and run; what their API answers is below.

**What does not**
- The level watcher does not start, so no level is ever up: `query` answers
  `levelup=0 epoch=0 level=- safemode=1` and no player keys, and `status` has
  no position or health lines.
- The switches that act on the game refuse to come on, at launch or live:
  `hitreact`, `npctuning`, `aituning`, `triggers`, `playertune`, `prefsedit`.
  `features` lists them as `refused`; `features.txt` keeps their lines.
- The console refuses every command that reaches the game, with one line and
  no effect:
  ```
  hp is disabled on this game build (unknown - safe mode): it uses addresses measured on the Steam release, and using them here could crash the game
  ```
  That is 150 of the 220 built-ins: every player, movement, items, world,
  tuning (but `features`) and music command, the debug ones that read game
  structures, `call` and the game's functions by name, and the `pointers.txt`
  chains (`ptr`, `get`, `hold`, `unhold`, `ptrreload`, and `set` on a chain).
  `help` marks each with a `*`, and `help <command>` says it is unavailable.
- `menu` refuses its keyless half (`menu list`, `continue`, `skip`, `resume`,
  `fs`); `continue` falls back to key presses.
- Plugins: the API calls that would reach the game answer
  `SWSE_E_UNSUPPORTED` (`Game.PlayerPos`, `PlayerYaw`, `SetPlayerYaw`,
  `PlayerHealth`, `PlayerStamina`, and `Memory.Write`) or 0 (`Game.HashPath`,
  `ResourceLookup`), and no level event arrives. `Memory.Read`, and the GL,
  frame, console, event and file calls, work as usual.

**Seeing it.** `status` says safe mode is on and what works; `hooks` lists the
patches it refused; the self-test runs once, 10 seconds after the game starts
drawing, heads its report `unknown build: safe mode`, and names the systems
that do not run on a `[SAFE]` line, never as a failure.

`SWSE_UNKNOWN_BUILD_OK=1` in the game's environment turns safe mode off, for
testing a build once its addresses have been checked - at your own risk:
everything then runs, at addresses measured on another build.

---

## Native plugins (1.1)

A mod can ship code. A plugin is a 32-bit DLL in a mod folder's `plugins\`
subfolder - `SWSEMods\<Mod>\plugins\<name>.dll` - that SWSE loads and calls
through a small C API, `sdk\swse_plugin_api.h`. Writing one is covered in
[sdk/README.md](sdk/README.md), which ships a complete example; the design and
the reasoning behind each rule are in `swse/research/PLUGIN_SYSTEM.md`.

**Each plugin is a switch, off until you name it.**
- `hello.dll` is the switch `hello`: `features hello on` (saved to
  `features.txt`), `features hello on temp`, or `hello = on` in the file.
- While its switch is off, SWSE does not even load the DLL. A plugin dropped
  into a mod folder runs no code until someone asks for it by name.
- `features all on` never switches a plugin on. `features all off` switches
  them off too.
- Switched off, a plugin's callbacks stop, but its DLL stays loaded until the
  game exits: code the game may be running cannot be unloaded safely. A later
  `features hello on` calls it again without reloading it. To try a rebuilt
  DLL, restart the game.
- A plugin cannot take a built-in switch's name or nickname (a `graphics.dll`
  is refused), nor `all` or `preset`, and it is never `auto`.
- Nicknames a plugin offers for its switch work once it has loaded.

**Where plugins come from.** They are found through the mod registry, like
every other mod file:
- `load_order.txt` orders and disables a plugin with its mod (`!My Mod`).
- When two enabled mods ship the same name, the one later in load order is
  used, and `plugins` shows the other as shadowed.
- `mods reload` (or `plugins rescan`) finds plugins added while the game runs.
  They appear as new switches, off.

**Commands.**

| Command | What it does |
|---|---|
| `plugins` | every plugin found: state (`on`, `off (loaded)`, `off`, `FAULTED`, `REFUSED`), version, mod, frame cost, commands, SHA-256 |
| `plugins <name>` | one plugin: file, plugin API, full SHA-256, what it registered, how much it has run, why it faulted or was refused |
| `plugins rescan` | look for new plugins now |
| `query plugins` | `<name>=on\|off\|faulted\|refused` for every plugin, for tools |
| `help <plugin>` | a plugin's commands, described |
| `hooks` | every code or table patch SWSE has live, and the system that owns it |

A plugin's commands work everywhere a built-in's do: typed, from key binds,
aliases, `scripts\`, trigger `do =` lines and the mailbox. They resolve after
the built-ins and before scripts, aliases and the game's own functions, so a
plugin can never replace a built-in command. When two plugins offer one
command, the one later in load order runs it, whichever was switched on first.

**When a plugin misbehaves.**
- Every call SWSE makes into a plugin is guarded. A fault - a bad pointer, a
  C++ exception escaping, a stack overflow - switches that plugin off for the
  rest of the session. The game keeps running, and the console and
  `swse_log.txt` name the DLL and the offset: of the faulting instruction, of
  the code that threw, or of the plugin's own call when the fault was inside
  something it called.
  ```
  plugin 'crashy' FAULTED: exception C0000005 in a command at crashy.dll+0x1019 - switched off for this session
  plugin 'crashy5' FAULTED: exception C0000005 in a command at ntdll.dll+0x..., called from crashy5.dll+0x... - switched off for this session
  ```
- `features.txt` is left alone, so a fixed build loads normally at the next
  launch. Until then `features <name> on` is refused: the plugin's state may
  be half-updated.
- Not caught: a fault on a thread the plugin started, heap corruption that
  crashes the game later, and a plugin that never returns. The guard covers
  SWSE's own calls on the render thread only.
- Every call is timed. `plugins` and `perf` show each plugin's frame cost, and
  a `FRAMESTALL` line in the log names the plugin that owned a slow frame
  (`-> SWSE, plugin crashy6 121 ms`).
- The self-test lists each plugin that is on (its own checks, if it has any),
  FAIL for one that faulted, and those that are off on the `[OFF ]` line.

**Trust.** A plugin is native code with the game's full rights, and nothing
inside the process can sandbox it. What SWSE does instead:
- it runs nothing until you name the plugin;
- it writes the DLL's path and SHA-256 to `swse_log.txt` before any of its
  code runs, and `plugins` shows them, so a bug report says exactly which
  build ran;
- it shows an unloaded plugin's description and version from the DLL's
  version resource, read without running it.

Install plugins only from people you trust.

**Safe mode.** On a game build SWSE does not recognise, plugins still load:
their API reaches the game only through SWSE, and SWSE gates those calls there
(see [Game builds and safe mode](#game-builds-and-safe-mode-11)).

**No plugins, no change.** With no plugin switched on, no plugin code runs
and nothing is hooked on a plugin's behalf. Plugins receive GL notifications
(texture binds, texture uploads) from SWSE's own hooks rather than patching
GL themselves: SWSE's GL hooks restore the original bytes on every call, and
would silently erase a second patch laid over them.

---

## Driving the console

Two ways in:

* **In game** - the `` ` `` / `~` key opens the console. Tab completes,
  PageUp/PageDown scroll, Enter runs, Esc closes. Up/Down recall the lines
  typed this session, and the clipboard works (1.1.1, below). While it is open the game
  gets no key presses, mouse movement, clicks or wheel (1.1), so typing never
  walks Stranger and the mouse does not turn the camera. That holds for keys
  the game reads just after the console closes, such as the key that closed
  it. Releases still pass, so nothing is left held down. The console reads the
  keyboard only while the game is the window you are in, so typing in another
  app never lands in its line, even with it open (after a mailbox `show`).
* **From outside** - write a request into
  `SWSEMods\SWSE Console\remote_in.txt` and read the reply back from
  `remote_out.txt` (the mailbox, below). It needs **no window focus**, which
  matters: the console reads the keyboard with `GetAsyncKeyState`, so
  synthesising keystrokes would type into whatever window is actually focused.

### The console's look and keys (1.1.1)

* **The header.** `SWSE` and the version at the left; at the right, the frame
  rate as plain text, `62 fps  16.1 ms`, averaged over half a second from
  SWSE's own frame counter. It is measured whether the console is open or
  not, so a number is there the moment it opens.
* **Text.** Rasterized at the window's size (Consolas 20 px at 1080 lines, 27
  at 1440; Bahnschrift, else Segoe UI, for the header and the card) and
  rebuilt when the window changes. Lines are coloured by type: your commands
  in gold with the time, errors on a red row, results in green, usage and
  `(none)` dimmed. A scrollbar shows when there is more than fits.
* **Suggestions.** While a command name is typed, a card under the console
  lists every command it could become, with its help: the first one shows as
  ghost text after the cursor. Tab takes it, Up/Down choose (Enter then runs
  the chosen one), Esc hides the card and a second Esc closes the console.
* **History.** Up/Down, with no card showing, step through the lines typed
  this session (32, no repeats in a row). Lines run by `exec`, the mailbox or
  a bind are not added.
* **Paste - Ctrl+V or Shift+Insert.** As in a terminal: the text goes in at
  the end of the line, and each line break runs the line before it, in order
  (through the same runner as `exec`, so `wait` delays the rest; 64 lines at
  most; `#` lines and blank ones skipped; each run line joins the history).
  What follows the last line break stays in the input. Text without a line
  break just goes into the input. Unicode becomes ASCII: curly quotes, dashes
  and non-breaking spaces become their plain forms, anything else `?`.
* **Copy - Ctrl+C / Ctrl+Shift+C.** Ctrl+C copies the line typed; with
  nothing typed, the last command and its output (from its `> ` line to the
  end). Ctrl+Shift+C copies the lines on screen. What happened shows in the
  input field for 2.5 s, not in the scrollback.
* The clipboard keys work only while the console is open and the game is the
  window you are in, as for typing. No other Ctrl+key types anything; Ctrl
  with Alt is AltGr, so AltGr characters type on European layouts.
* `help` ends with these keys. No command was added or changed: 1.1.1 has
  1.1's 220 commands, word for word.

`help` lists everything by category (player, movement, items, world, tuning,
graphics, raytrace, input, scripting, debug, music, console); `help <category>`
or `help <command>` narrows it. `clear` empties the console and `echo <text>`
prints a line.

A typed name is looked up in this order: a built-in command, a `scripts\*.txt`
command, an alias, then one of the game's own 181 script functions.

**Sequences (1.1).** `;` runs several commands from one line:
`heal; wait 500; status`. `wait <ms>` delays everything after it - the rest
of the line is handed to a scheduler, so a sequence never blocks a frame. Lines
that begin with `alias`, `bind`, `after` or `repeat` keep the whole rest of the
line, `;`s included, as the command they store. Up to 32 parts per line.

### The remote mailbox (for tools)

The console polls `SWSEMods\SWSE Console\remote_in.txt` about eight times a
second. The first line is a sequence number; every following line (up to 256,
each up to 239 characters) is run exactly as if typed, as one sequence, and
lines starting `#` are skipped. A request runs when its sequence number **or
the file's write time** changes, and the first poll after launch only records
the file already on disk, so a leftover request never replays itself. Checking
the write time too means a tool that restarts its numbering every session is
not ignored; a new number per request is still the clean way - for example the number on disk plus one.

**End the file with a newline.** A request runs only once it is completely
written: when the file ends with a newline, or has sat unchanged for one poll
(about 120 ms) for a writer that leaves the last newline off. A request caught
mid-write would otherwise run in part and then again in full. If lines were
cut or dropped, the reply says so.

The reply goes to `remote_out.txt`:

```
<seq>
> first command
...its output...
> second command
...its output...
<<END>>
```

`<<END>>` marks a finished reply, so a reader can tell it from one caught
mid-write. A command that faults writes `*** FAULTED - command crashed (game
survived) ***` instead of taking the game down. A `wait <ms>` line delays the
lines after it; those, and anything scheduled with `after`, run later and
print to the console, not into the reply. The mailbox belongs to the console
feature: with `console = off` there is none, and `remote off` stops it for the
session (`remote` shows its state and the last sequence number).

### status, query, mods (1.1)

| Command | What it does |
|---|---|
| `status` | for people: SWSE version, level up or not (with its epoch and age), position and facing, health / stamina / moolah, which features are on. In safe mode: the game build and what works, instead of the player lines |
| `query [player]` | one line of key=value: `levelup epoch level x y z yaw health healthmax stamina staminamax moolah`; in safe mode `levelup=0 epoch=0 level=- safemode=1` |
| `query position` | the same without health, stamina and moolah |
| `query features` | `console=on graphics=off ...` for every switch (eleven) |
| `query version` | `swse=1.1.1 build=Steam_HD`; `build=unknown` on a game build SWSE does not recognise, which runs in safe mode |
| `query contract` | `contract=1 swse=1.1.1`: the version of the tool contract below |
| `mods` | enabled mod folders in load order, and which SWSE files each provides |
| `mods reload` | rescan `SWSEMods` and `load_order.txt`, reload positions and scripts, and re-read the files of the tuning systems that are on (`aiprefs.txt`, `triggers.txt`, `playerprefs.txt`, `prefs.txt`, `console.txt` / `characters.txt`) |
| `hide` / `show` | close / open the overlay (a tool on the mailbox cannot press `~`) |
| `log <text>` | write `LOG: <text>` into `bin\swse_log.txt` |

`query` fields appear only when they can be read: no `x=` before a level has
loaded. `level=` is `-` unless SWSE knows the level name (see
[Positions and facing](#positions-and-facing)). Coordinates carry three
decimals, yaw two.

**Writing a tool?** [TOOL_CONTRACT.md](TOOL_CONTRACT.md) lists what
holds for every 1.x release: finding SWSE and the game folder, the mailbox
protocol, the `query` keys, the switches, and the files a tool may write.
Parse `query`, not console prose - the prose may change in any release.

---

## Scripting and automation

The game ships a script VM, and SWSE can call into it.

| Command | What it does |
|---|---|
| `list [filter]` | 181 discovered game functions |
| `call <function> [args]` | call one (or just type its name) |
| `scripts` / `reloadscripts` | your own `.txt` command files |
| `ptr` / `get` / `hold` / `unhold` / `ptrreload` | named pointer chains (`pointers.txt`) |
| `exec <file>` | run a text file of commands (1.1) |
| `after <sec> <command>` | run a command later (1.1) |
| `wait <ms>` | pause inside a sequence, alias or script (1.1) |
| `repeat <n> <command>` | run it n times now, up to 50 (1.1) |
| `alias <name> <cmd; cmd>` / `unalias` / `aliases` | make a command from others (1.1) |
| `bind <key> <command>` / `unbind <key>\|all` / `binds` | run a command from a key (1.1) |

**Script commands.** Any `.txt` file in `SWSE Console\scripts\` becomes a
command named after the file; each line runs as if typed, and in 1.1 a
`wait <ms>` line delays the rest. Up to 48 scripts of 24 lines. The folder is
the `scripts\` of the last enabled mod that has one.

**exec** reads a file relative to `SWSEMods\SWSE Console` (or an absolute
path), up to 64 lines, `#` comments, `wait` honoured.

**Nesting limits.** `exec`, `repeat`, aliases and script commands can call
each other up to 8 deep. One line - typed, mailed, bound or scheduled - may
expand to at most 2,000 commands in all, then the rest are skipped with a
message. A file that execs itself, or `repeat 50 repeat 50 repeat 50 ...`,
stops there instead of crashing or freezing the game.

**Aliases** (`aliases.txt`): `alias fullheal hp 1000; stam 500`. `%1`..`%9`
are the alias's arguments and `%*` all of them. An alias cannot shadow a
built-in (built-ins are looked up first, and `alias` refuses the name).
Commands can nest up to 8 deep.

**Key binds** (`binds.txt`): `bind F5 savepos`. Keys are `F1`-`F24`, `A`-`Z`,
`0`-`9`, `numpad0`-`numpad9` (or `kp0`-`kp9`), `insert`, `delete`, `home`,
`end`, `pgup`, `pgdn`, the arrows (`up` `down` `left` `right`), `space`, `tab`,
`enter`, `backspace`, `pause`, `scrolllock`, `minus`, `equals`, `lbracket`,
`rbracket`, `semicolon`, `quote`, `comma`, `period`, `slash`, `backslash`,
`kpmultiply` `kpadd` `kpsubtract` `kpdivide` `kpdecimal`, and the mouse
buttons `mouse3` (middle), `mouse4` and `mouse5`, with `ctrl+`, `alt+`,
`shift+` prefixes. A bind fires with exactly those modifiers held,
and only while the console is closed and the game really has focus.
- **Shift fallback:** with Shift alone held, a key that has no `shift+` bind
  runs its plain bind.
- **Ctrl and Alt** combinations always need a bind of their own.
- **Not bindable:** `~` and Esc.
- **A bound key still reaches the game too** - pick keys it does not use.

Alias names are up to 31 characters.

Both files live in `SWSE Console`, are read once when the console starts, and
are rewritten by `bind`/`unbind` and `alias`/`unalias` from what is loaded -
so edit them with the game closed or use the commands, not both. A rewrite
keeps every `#` comment line already in the file (ahead of the entries), and
is written beside the file and swapped in, so a crash cannot empty it. The
release ships commented examples in `SWSE Console\examples\`.

---

## Positions and facing

| Command | What it does |
|---|---|
| `pos` | your X Y Z and facing |
| `yaw [deg]` | read or set the way you face (1.1) |
| `savepos` | quick slot: position **and** facing (in memory, one slot) |
| `tp` | back to the quick slot, facing restored |
| `savepos <label>` | the same as `writepos <label>` (1.1) |
| `tp <label>` | the same as `goto <label>` (1.1) |
| `tpxyz <x> <y> <z> [yaw]` | teleport to coordinates (1.1) |
| `up [dist]` | lift yourself exactly `dist` along Z (default 10) and fall back down - no ceiling check, so use it in the open |
| `move <axis 0\|1\|2> <delta>` | step along one axis: X or Y lands you on the floor there, 2 (Z, up) puts you exactly there |
| `writepos <label>` | save where you stand and your facing, by name, to file |
| `goto <label>` | teleport to a saved position and facing |
| `positions` | list them, with yaw and level |
| `spawnat <label\|here> [n] [type]` | move existing NPCs to a position (they arrive without their AI running) |

Since 1.1 every teleport is the game's own - the call its checkpoint respawn
makes - so the destination streams in first (a short hitch on a long jump).
The destination needs a zone: `tp` uses the one `savepos` recorded, anything
else the zone whose box contains the point, and a point outside every zone is
refused. Refused too while a level loads, in a boat, and on an unknown game
build. Before 1.1 no teleport moved the player at all.

**Known risk: teleporting into a scripted fight can crash the game.** Seen
twice in testing after 1.1's release: a teleport into an area where the
level's script starts a fight, and the game closed a few seconds later (a
fault inside the game's own combat script). Save first, and walk into story
fights rather than teleporting into them.

**Yaw** is the rotation angle of the player's world matrix about Z (up), in
degrees 0..360 - `atan2(m3, m0)`, **the same angle the game's level records
store for every placed object**, and the one Stranger: Armed to the Teeth
builds its placement matrices from, so a yaw copied between SWSE, AT3 and a
level record faces the same way. In compass terms **0 = facing -Y, 90 = +X,
180 = +Y, 270 = -X**: the model's forward is its local -Y, so the heading is
yaw - 90. The facing is not a field of the player or its motion object: it is
the rotation part of the player's geometry world frame, set through the
engine's own `Actor::SetFacing`, and nothing rewrites it per frame - so a set
facing sticks until something turns the player (movement, first person,
sniper view, pipes, boats). Details: `swse/research/PLAYER_FACING.md`.

**positions.txt** - one label per line, `#` comments, read from **every**
enabled mod (a later definition of a label wins):

```
label   x y z   [level]   [yaw]
enemyambush1   1250.50 40.20 -300.00   lm_level_03   270.00
```

The two columns after z may come in either order - a number is the yaw, a word
is the level, `-` means none - so 1.0.x files load unchanged, and 1.0.x reads
1.1 files (it only looks at the level column).

**sites.txt** - the same points in the column order Stranger: Armed to the
Teeth reads for placements, `label x y z yaw [level]`. Also read from every
enabled mod.

**`writepos`** appends the point to `SWSE Console\positions.txt` and, **only
when the facing could be read**, to `SWSE Console\sites.txt` - a made-up 0
there would be read as a facing. Appending keeps the author's comments, and
re-using a label simply moves the point. Every mod's `positions.txt` is still
read, so a mod author moves the lines into their own mod. (1.0.x appended to
whichever mod last had a `positions.txt`.)

**The level column is partial.** SWSE learns the level name only from its own
`warp` command, so after loading a save, or a normal in-game transition, the
column is written as `-`; and if you warped earlier in the session, then
changed level another way, the name can be stale. `goto` warns when a label
was saved in a level other than the one SWSE thinks you are in.

---

## Player tuning - `playerprefs.txt` (1.1)

The player's own numbers as a file, applied on every level while the
`playertune` feature is on:

| Key | What it sets |
|---|---|
| `health` | max **and** current health (all three stored copies) |
| `stamina` | max and current stamina |
| `speed` | run speed - the player's own motion objects |
| `jump` | jump height - the player's own motion objects |
| `gravity` | fall gravity - `GlobalMotionPrefs`, so **everyone**, NPCs included |
| `aircontrol` | midair steering - `GlobalMotionPrefs`, everyone |

`key = value` or `key value`. A key that is blank, `-`, `default` or absent
keeps the game's own value. **The release ships no `playerprefs.txt`, only a
template in `SWSE Console\examples\` that leaves every key blank**, so a stock
install changes nothing (and `playertune`, which ships `auto`, stays off). This
is exactly the file Stranger: Armed to the Teeth writes
for its Player Health and Player Stamina fields; the other four are SWSE
additions the same file can carry. The last enabled mod that provides a
`playerprefs.txt` wins (normally `SWSE Console`).

**When.** Once per level, 3 seconds after the level watcher sees the player
appear - after the game's own difficulty initialisation (600 / 300 / 150
health for easy / normal / hard) rather than racing it. If the game later puts
max health or stamina back to its own number (a checkpoint reload
re-initialising the player, say), the value is applied again; a value changed
by anything else - `hp`, an artifact - is left alone. Writing health or stamina
also fills the bar to the new maximum.

Every value is captured before the first write in a level, so it can be put
back exactly.

| Command | What it does |
|---|---|
| `playertune` | each key: wanted, live, and the game's own value |
| `playertune reload` | re-read the file and apply now |
| `playertune apply` | apply now |
| `playertune restore` | put the game's own values back (as does switching the feature off) |

The commands work with the feature off; only the per-level automatic apply
needs it. `hp`, `stam`, `speed`, `jump`, `gravity` and `aircontrol` read the
current values.

---

## The live prefs editor - `prefs`, `prefs.txt`, `knockback` (1.1)

Every tunable in the game - ammo knockback and damage, NPC health, weapon
timing, motion, explosions - is a prefs record (`/data/prefs/**.txt`) that the
game loads into an ordinary C++ object. Offline tools reach them by patching
the serialized records inside the `.smb` bundles on disk. The prefs editor
reaches the **live objects**: nothing on disk changes, nothing needs a backup,
and the game's own files are never touched.

**Targets**

| Form | Meaning |
|---|---|
| `/data/prefs/weapons/damagedynamite.txt` | a prefs path, hashed with the game's own hasher (case and `/` vs `\` do not matter) |
| `weapons/damagedynamite` | shorthand for the same path |
| `1B2A36F7` | a path hash, 8 hex digits |
| `@NPCWeaponPrefs` | every live object of an RTTI class |
| `&1A2B3C40` | one object by address (console only - addresses change) |

A path or hash is looked up in **the game's own resource registry** - a sorted
list of every loaded resource (6,416 in `lm_level_01`), binary-searched,
read-only, nothing called or created - and a heap scan is used only if the
registry cannot be read. `prefs find` says which it used. A class target is
always a heap scan.

**Fields**

| Form | Meaning |
|---|---|
| `m_maxKnockSpeed` | a reflected field name, from `swse/reflect_gen.h` (5,697 fields in 191 classes; every class lists its inherited fields) |
| `NPCPrefs::m_health` | class-qualified, when a name is ambiguous |
| `m_spAIPrefs.m_sightCombat.m_seeDistance` | a field of an embedded object, dotted (here the combat sight distance inside an `NPCPrefs`) |
| `m_someVector.y` | one component of a vector field (`.x .y .z .w` or `.r .g .b .a`) |
| `0x448` or `+448` | a raw byte offset |
| `...:i` `:b` `:h` `:f` | type suffix: int, byte, hash, float (float unless the table says otherwise) |

A name resolves against the object's own RTTI class first; otherwise the bare
name is accepted only when every class that lists it agrees on the offset, and
an ambiguous name is reported with the candidates rather than guessed.

**Values** are numbers, and the whole token must be the number:
- `12`, `-1`, `0.5`; `0,5` also works (a decimal comma).
- `010` is ten; `0x10` is hex.
- A hash field takes 8 hex digits or a `/data/...` path.

**Fields that are never written.** A struct, vector or array field (`prefs
dump` shows these as `struct`), or a string handle, is refused unless you add a
type suffix - and add one only if you know the layout. A number written into a
struct corrupts it, and a raw value in a string handle crashes the game when it
next uses the string.

`keep` saves a line to `prefs.txt` only when the field and value were
accepted, or when the record is simply not loaded in this level. A line that
is refused is never saved, because it would be refused again at every level
load.

| Command | What it does |
|---|---|
| `prefs` / `prefs status` | how many edits are loaded, how many values are changed and restorable, the last apply |
| `prefs find <target>` | the live objects: address, class, own hash, and whether the registry or a heap scan found them |
| `prefs get <target> <field>` | read a field |
| `prefs set <target> <field> <value>` | write it now (until the level reloads its objects) |
| `prefs keep <target> <field> <value>` | write it now **and** add the line to `SWSE Console\prefs.txt` |
| `prefs dump <target> [filter]` | every known field of the object's class, with values |
| `prefs fields <filter>` | search the reflected field names |
| `prefs restore` | put every changed value back |
| `prefs reload` | re-read every `prefs.txt` and apply now |
| `prefs registry [path\|hash]` | diagnostic: the registry's shape, and one lookup traced step by step |

**prefs.txt** - `<target> <field> <value>` per line, `#` comments anywhere,
read from every enabled mod (up to 256 edits in all) and applied on a worker
thread 4.5 seconds after each level's player appears, while the `prefsedit`
feature is on. `prefs keep` replaces an earlier line for the same target and
field rather than stacking them. The release ships no prefs.txt (an update
would overwrite yours); a commented example is in
`SWSE Console\examples\prefs.txt`, and `prefs keep` creates the real file.

**Knockback.** Every player ammo type is a prefs record at
`/data/prefs/weapons/<name>.txt` (`BoltDamagePrefs` and its siblings). The two
knockback fields belong to `BoltPrefs`, which every ammo type and every NPC
weapon inherits: `m_maxKnockSpeed` (how hard it throws what it hits) and
`m_maxKnockSpeedPlayer` (how hard it throws **you**). The engine uses
the player value when it is 0 or more and `m_maxKnockSpeed` otherwise, and only
knocks when the result is above 0 - so the shipped `-1` means **the same as for
NPCs**, and `0` means **never knocks you**.

| Command | What it does |
|---|---|
| `knockback` | all 31 ammo types with their current values |
| `knockback <ammo\|all> <npc\|-> [player\|-] [keep]` | set them; `-` leaves one alone; `keep` also adds the lines to `prefs.txt` |

Ammo is named by its file name or a nickname: `thudslug` (`damagearmadillo`),
`riotslug`, `stingbee`, `superstingbee`, `fuzzle`, `rabidfuzzle`,
`fuzzlepounce`, `rabidpounce`, `bolamite`, `bolablast`, `sniperwasp`,
`stunkz`, `sparkstunkz`, `boombat` (`damagedynamite`), `boombatseekers`,
`chippunk`, `howlerpunk`, `hivequeen`, `hivequeen2`, `punch`, and numbered
nicknames for the loader / extender variants (`thudslug1`, `boombat2`... -
`knockback` lists them all). Examples: `knockback boombat 40 25` - boombats
throw what they hit at 40 and you at 25; `knockback all - 0` - nothing you
fire can knock you back. Without `keep` it lasts until the level reloads.

**Safety.** Every original value is captured, with the object's vtable and own
hash, before the first write; `prefs restore` and switching the feature off
write it back only if the object is still the same one, so a freed address
reused by something else is never written. A class-wide target is weaker
evidence than a hash, so a float is written through one only if the value
already there is a plausible number, and thread stacks are never scanned.
Verified live: `knockback boombat 40 25`, `knockback all - 12`, then
`prefs restore` - 32 values restored, all back to vanilla.

How objects are found, and why the ammo offsets from bundle work do not apply
to live objects: `swse/research/PREFS_EDITOR.md`.

### Characters and weapons by name - `npc`, `wpn` (1.1)

Friendly fronts on the prefs editor for the two records people edit most,
built from the proposals in `swse/research/AT3_DISCOVERIES.md`. Every write
goes through the editor, so `prefs restore` undoes it and `keep` adds it to
`prefs.txt` to re-apply every level.

| command | does |
|---|---|
| `npc <type>` | the main fields of a character: health, stamina, species, gib, gib effect, gib-spawn, hurt reaction, bounty, death loot, mass, run speed, jump, sight (normal/combat), relax time |
| `npc <type> <field> [value] [keep]` | read or set one field |
| `npc <type> dump [filter]` | every reflected field of that character with its value |
| `wpn <ammo\|npcweapon\|hash>` | the main fields of a weapon: damage, damage to objects, clip, total ammo, area of effect, range, speed, gravity, homing, bounce, knockback (NPC/player), fire rate, reload |
| `wpn <...> <field> [value] [keep]` | read or set one field |
| `wpn all <field> <value> [keep]` | every player ammo type at once |

`<type>` is a character's 8-digit type hash (`npcguns`, `types`, `npcnear`) or
one of the recovered names written without spaces (`outlawshooter`, `heavy`,
`outlawcutter`...). A weapon is an ammo name or nickname (as `knockback`), an
NPC weapon name (`outlawshooter`, `wolvarkshooter`, `slogbolt` - i.e.
`/data/prefs/weapons/npc/<name>.txt`), a weapon's hash, or a CHARACTER's hash,
which means that character's ranged weapon.

`npc` fields: `health stamina healthregen staminaregen exhaust species gibfx
gibsound gib gibspawn hurt moolah capturemoolah bountyable melee ranged
ammorule lootlimit loot.damage loot.exhaust loot.death loot.steefram loot.ram
loot.steeframdead loot.ramdead mass turn walk trot canter run jump pushable
knockable fade aimradius relax allowpanic`, plus
`sight.<normal|agit|combat|panic>.<6th|see|above|below|hangle|vangle|instant|hidevol>`
and `attack.<param>` (the 42 combat tactics - `npc <type> dump attack` lists
them), or any reflected `m_` name or hex offset.

`wpn` fields: `damage dmgobj stamina clip clipmin total aoe aoeedge blast range
speed gravity homing bounce knock knockplayer gibkill duration immobilize
firerate reload reloadmax accuracy misstime kick`, or any `m_` name. `firerate`
and `reload` pick the right member for player ammo (`m_fireRateNormal`...) and
NPC weapons (`m_fireRate`...).

Values: numbers; hashes as 8 hex digits or a `/data/...` path; `on`/`off`;
`none` (the engine's unset token). Examples: `wpn thudslug homing 360`
(homing thudslugs), `wpn outlawshooter damage 5`, `npc outlawcutter gib on`,
`npc heavy sight.combat.see 300`, `npc outlawcutter loot.death none`.

Verified live (lm_level_01): `npc outlawshooter` and `wpn thudslug` /
`wpn FFFC00CB` (the shooter's gun: damage 45, 1.3 shots/s) read correctly;
writes to homing, a sight distance and the gib flag applied, and `prefs restore`
put all three back. Which fields take effect on characters ALREADY in the level
varies: some are read live, some are copied into an NPC when it is created and
only reach ones spawned later - `keep` plus a level load settles it.

---

## Level watcher (1.1)

"A level is up" is its own signal now: the player object plus its two form
motion objects (Stranger's and Steef's), polled four times a second. A level
load rebuilds them, so a changed set is a new level - an **epoch**. Changing
form only swaps which motion is current, and leaves both in place, so it is
not a new level; and the player must be missing for 1.5 s before a level
counts as gone, so one missed poll is not either. Everything that re-applies
per level keys off the epoch and waits a settle delay after it, so the level's
cast has finished building first:

| Applier | Delay after the player appears |
|---|---|
| player tuning | 3 s |
| AI tuning (`active` profile) | 4 s |
| triggers (`levelload`, and evaluating triggers at all) | 4 s |
| prefs edits | 4.5 s |
| self-test | 5 s |

In safe mode (an unknown game build) the watcher does not start - finding the
player means calling the game - so nothing that waits for a level runs.

1.0.x used the hit-reaction system's actor list for this, which only fills
while hit reactions are on - so with them off, `active = <profile>`, every
trigger and the automatic self-test silently never ran. Triggers that count
NPCs (`killed`, `cleared`) now keep their own NPC list when hit reactions are
off.
- **Background thread:** the list is built by a heap scan on a background
  thread, never on the render thread.
- **After a level change:** it is rescanned at once.
- **During a level:** it is rescanned every 20 s while a trigger needs it,
  because the game does create NPCs mid-level (gib spawns, spawn-pool
  refills). Between scans, dead entries are dropped every half second.
- **Commands:** `npccache` shows the list and how long the last scan took;
  `npccache rebuild` forces a rescan.

Verified live: `levelload`, `every` and `cleared` triggers all fire with hit
reactions off.

**Switching triggers on mid-level** (`features triggers on`) does not fire the
current level's `levelload` triggers, provided the level loaded at least 4 s
earlier. Its per-level counts start from the switch.

---

## Graphics

### RTGI / screen-space global illumination

A real post-process pass injected at `wglSwapBuffers`, using the game's own
scene depth and colour buffers. **Needs the `graphics` feature**; with it on,
the look still starts off each launch until F10 or `gfx on`.

| Command | What it does |
|---|---|
| `gfx on\|off\|toggle\|reload` | enable the post-process pass (refuses while the feature is off) |
| `set <key> <value>` | live-tune any setting; the value is also written into `graphics.txt`. A key SWSE does not read, or a value that is not a number, is refused and nothing is written |
| `depthtex [id\|auto]` | list / pin the scene depth texture |
| `proj [scan]` | the engine's real near / far / FOV |
| `fbotrace [n]` | log the frame's FBO bind order |
| `framerate` | fps since the previous call |
| `perf` | where a stutter came from - SWSE or the game |

F10 toggles the look, F11 reloads the settings. Settings live in
`SWSEMods\SWSE Graphics\graphics.txt` - any mod may provide a `graphics.txt`
and the last enabled one wins, so a mod can ship a whole look.

Finding the right depth buffer was the hard part: the picker rejects buffers
over 12M pixels on purpose, because the supersampled buffer looks plausible and
breaks GI everywhere. Still open: the first-person weapon can be shaded as the
ground behind it, because it is not in the scene depth the pass samples - see
"UI and the first-person weapon get shaded with the world behind them" and
"The weapon's depth was never gone" in `swse/research/GRAPHICS_RTGI.md`.

With graphics on but failed to initialise, a small gold square is drawn in the
top-left corner to prove SWSE still owns the frame. (1.0.x drew it whenever the
pipeline was absent, including when graphics had been switched off on purpose.)

### Ray tracing (experimental)

**New in 1.1, experimental. Needs the `graphics` and `raytrace` features.** SWSE builds a bounding
volume hierarchy (BVH) from the world geometry the game draws, and traces
ambient-occlusion rays through it on the GPU, feeding the post-process
composite. It is **not finished**: see the known problems below.

To see it:
- `features graphics on`, then `features raytrace on`. `raytrace` refuses to
  come on without `graphics`.
- `set rtao_enable 1`, which also writes it into `graphics.txt`, or the line
  `rtao_enable 1` there yourself (settings are `key value`, with no `=`).
- F10 or `gfx on` for the look.

`rtao_enable` alone no longer starts the tracer: the `raytrace` switch is the
master switch.

| Command | What it does |
|---|---|
| `rt [build]` | BVH and camera-trace status; `build` rebuilds the BVH now |
| `rt live [0\|1] [n]` | keep harvesting as new meshes are drawn, rebuilding every `n` (150) |
| `rt cardsize <area>` / `rt cardopacity <0-1>` | leaf cards: the triangle area above which a triangle counts as a card (2; then `rt build`), and how solid a card is to rays (0.12; 0 = transparent) |
| `rt <probe> ...` | measurement probes (`mode`, `nearz`, `flipy`, `fovscale`, `fovsweep`, `camerr`, `pairprev`, `aoclamp`, ...), used by the `RT_DIAG` notes |
| `harvest [show\|dump N]` | the world triangle soup the BVH is built from |
| `geo [show]` | capture the frame's draw geometry and check which space it is in |
| `gpu` | probe compute-shader and ray-math support |
| `progsrc <id>` | a shader program's position math |
| `ssrmask [reload]` | the screen-space-reflection material mask |
| `skin [show]` | census of this frame's character draws (debug) |
| `nohat [off\|<indexCount>]` | hide Stranger's hat, for inspecting the head mesh (debug) |
| `noponcho [off\|<n>]` / `nodreads [off\|<n>]` | hide Stranger's poncho or dreadlocks (debug) |
| `hidepart <indexCount>\|show <n>\|list\|clear` | hide any character part by the index count `skin show` prints (debug) |
| `meshdump [stop]` | capture character meshes for Oddview to `bin\swse_meshes.odv` (debug) |

`rt`, `harvest` and `geo` answer "ray tracing is off" while the switch is off.
The debug rows ride the graphics pipeline's draw hook, so they need `graphics`
on but not `raytrace`; without it they say so instead of doing nothing.

**Settings** (`graphics.txt`, or live with `set`):
- `rtao_radius` (4, in world units) and `rtao_rays` (8 per pixel);
- `rtao_strength` (1);
- `rtao_blend` (0.12), the temporal weight of the new frame;
- `rtao_dn`, `rtao_dn_radius`, `rtao_dn_depth`, `rtao_dn_normal`, the denoiser;
- `rtao_requirehit` (1): no BVH surface means unoccluded rather than guessed;
- `rtao_histflip`, `rtao_flipv`: orientation fixes, kept for measurement.

**Known problems.** The occlusion is wrong in ways that are understood:
- The BVH is harvested from what the camera already drew. Anything culled
  off-screen casts no occlusion, and the scene changes as you turn.
- Temporal accumulation has no world-stable identity to follow, so it
  smears.
- Alpha-tested foliage cards appear as solid slabs in the only live
  measurements. The card controls above have not been measured against them
  yet.

These are not fixed in 1.1.1.

### HD texture replacement

**Needs the `hdtextures` feature.** Textures are swapped at GPU upload -
**archives are never modified**. Each vanilla texture is fingerprinted as
`fnv1a32(level0[:4096]) ^ w*73856093 ^ h*19349663`, and a replacement is loaded
from any enabled mod's `textures\<FINGERPRINT>.oft` if one exists (the pack
goes in `SWSEMods\SWSE HD\textures\`; a later mod wins for the same
fingerprint). With the feature off the lookup is skipped entirely and the
folder is never even indexed.

Currently **960 textures live**, out of **1,119 replaceable** in the game. The
rest are deliberate exclusions: UI, menus, `.swf` art, normal maps,
particle/NPC effects and sky, which either upscale badly or are not surfaces.
Because substitution happens at upload, a bad texture just looks wrong -
delete the file and it is gone.

`hd` reports installed / substituted / **failed**. The failure count matters
because a replacement that fails to load silently falls back to vanilla: it
looks completely normal in play, so nothing but a counter will ever tell you.

#### Transparent textures - the trap

**Do not pack an alpha-cutout texture without its alpha.** The engine uploads
these as `GL_COMPRESSED_RGBA_S3TC_DXT1` (DXT1 *with* 1-bit alpha), but the
export/pack path works in RGB, so a texture that relied on transparency comes
back fully opaque and every formerly-clear pixel renders solid black. On a
tree that means the whole branch card appears as a black shard - the canopy
looks like shattered geometry, which sends you hunting a vertex-program bug
that does not exist.

Names ending `_AT` (alpha test) are the obvious cases, but the check is exact
rather than name-based: a DXT1 block with `color0 <= color1` is in 3-colour
mode where index 3 means transparent.

```
python tools/alpha_check.py               # report
python tools/alpha_check.py --quarantine  # move offenders aside -> vanilla
```

Run it after **every** batch, with the game closed - a running game holds the
archives locked, and the tool then skips them and still prints a total.

**And read the "located N of M" line.** `alpha_check` once reported zero
offenders while never having looked at 718 of 981 installed files, because its
manifest list was hardcoded and the new batch lived in a new folder. That is
how 13 broken trees shipped. It now discovers manifests and warns loudly when
any installed file is unaccounted for, because *a check that cannot see the
files reads exactly like a pass*.

**The pipeline**, end to end:

```
hd_export.py    archives      -> PNG named by fingerprint
upscayl-bin     ultrasharp-4x -> 4x PNG
downscale.py    4x            -> 2x  (Ultrasharp is a native-4x model; 2x is
                                      what Upscayl's own 2x setting does, and
                                      stranger.exe is a 32-bit process)
seam_check.py   before + after-> which textures tile, and whether they still do
seam_fix.py     broken only   -> edge feather to restore the wrap
hd_pack.py      PNG           -> .oft
```

**Seams are measured, not assumed.** AI upscalers process images in padded
tiles and quietly break the edge wrap, which shows up as a grid of seams across
the ground. `seam_check.py` compares the left/right column difference against
the interior column difference; a seamless texture sits near 1.0. Run it on the
originals first to learn which textures were tiling at all - many never do.
In the most recent batch 41 of 609 lost their wrap and were repaired, leaving
**158 of 158 tiling textures verified seamless.**

### AI difficulty profiles

The game's own difficulty menu sends **one integer** and changes no AI
parameter at all (`EASY`/`MEDIUM`/`HARD` = `new_game` 0/1/2, decoded from
`difficulty_menu.swf` - see `swse/research/AI_SYSTEMS.md`). Everything that
decides how hard a fight feels lives in per-character prefs objects difficulty
never reads.

`SWSEMods\SWSE Console\aiprefs.txt` defines named profiles that reach
them. **It ships `active = off`, and the `aituning` switch ships `auto`, so
nothing is tuned until `active` names a profile.** Naming one switches the
automatic apply on at the next launch, or at once with
`features aituning auto`; `aituning = off` in `features.txt` overrides it.
Three worked examples are included to copy from: `keen` (light), `relentless`
(harder), and `obvious` (deliberately absurd, for checking the tuning is
landing at all).

| Command | What it does |
|---|---|
| `difficulty <name>` | apply a profile now |
| `difficulty off` | restore the shipped values exactly |
| `difficulty` | show what is active |
| `aitune` | reload `aiprefs.txt` live, re-applying the profile in force |
| `findai [ms]` | locate perception objects, print their sight values |
| `weapons [ms]` | every NPC weapon: fire rate, reload, accuracy, miss time |
| `npcguns [ms]` | which character carries which gun, with hp and bounty |

The commands work with the feature off (run `aitune` first so the profiles are
read); only the automatic `active` apply needs it.

With the feature on, `active = <name>` applies the profile **once per level
load**, 4 seconds after the level watcher sees the player - never on a timer,
and the scan runs on a worker thread so it cannot stall a frame. It must
re-apply per level because a level load builds fresh prefs objects; without
that the tuning silently lapses when you change area.

`aiprefs.txt` is a single-value file: the last enabled mod that provides one
wins. SWSE reads up to 32 profiles, from the first 64 KB of the file. A value
written with a decimal comma (`0,667`, as AT3 writes it on a French or German
Windows) is read correctly.

**Safe across level changes (1.1).** Every value is computed from the shipped
original, captured the first time an object is tuned.
- **Tied to a record:** each stored original remembers which prefs record it
  belongs to (the record's vtable and path hash, checked against the game's
  resource registry). It is used only while that record is still in memory.
  An original whose record has gone is dropped, and nothing is ever written to
  where the record used to be.
- **Kept while the record lives:** records that survive a level load keep
  their original. Re-applying therefore never compounds, and `off` restores
  the shipped numbers exactly.
- **Busy background apply:** the `active` profile is applied on a background
  thread. While it runs, `difficulty <name>` answers "busy", and `difficulty
  off` is queued and runs the moment the apply finishes.

1.0.x kept bare addresses. After a level change, `difficulty off` wrote into
freed memory, and a re-apply captured tuned values as "shipped".

**Values are multipliers, not absolutes** - all but one. Characters differ a
lot - one outlaw sees 50 units, another 115, another through a narrow 70°×110°
cone, and fire rates run 0.1 to 10 shots/second - so a flat number would erase
hand-authored variety.

**`misstime` is given in milliseconds, not as a multiplier (1.1).**
`m_missTime` is the window in which a shooter deliberately misses; it is stored
in seconds and ships at 0.010 (10 ms) on almost every NPC weapon. `misstime` is
written in the game's standard milliseconds - **10 is vanilla**, 20 doubles
every weapon's window, around 1000 enemies can barely land a shot - which is
the number Stranger: Armed to the Teeth writes. Each weapon scales from its
own shipped value (shipped × misstime / 10), so the odd weapon that ships
longer keeps its proportion and one that ships 0 stays 0. Only firing weapons
get it; `-1` or leaving it out keeps the shipped values, and `difficulty off`
restores them exactly. 1.0.x parsed the key and ignored it.

**Only enemies are tuned.** Weapons are reached *through* their owning
character (`NPCPrefs.m_rangedWeapon` joins to `NPCWeaponPrefs`'s own hash at
`+0x0C`), so anything at 100000 health - townsfolk, Clakkerz, natives - is
skipped by construction rather than by hoping. That still holds after
character tuning (`characters.txt`, `noimmortals`, `npchealth`) has made such a
character mortal: SWSE remembers every type it lowered from 100000 and keeps
treating it as protected. The player is never touched.

Live-verified perception defaults:

| | normal | agitated | combat | panic |
|---|---|---|---|---|
| `m_seeDistance` | 50 | 100 | 100 | 75 |
| `m_6thSenseDistance` | 10 | 10 | 10 | 10 |
| view cone | 90×90 | 90×90 | 90×90 | 90×90 |
| `m_hideVolSeeDistance` | 1 | 2 | 3 | 3 |

Sight **doubles** the moment an NPC is agitated - the shipped model, measured.

Two properties tested rather than hoped for: every apply is computed from a
stored baseline, so **applying twice does not compound**, and `difficulty off`
lands back on the exact shipped numbers.

**Units matter, and two of them are counter-intuitive.** `m_fireRate` is a
rate in **shots per second** (higher = faster), and `m_accuracyWidth` is a
**spread** (lower = more accurate). Both were documented backwards at first;
the fire-rate error was only caught in play, because the numbers were
self-consistent and wrong.

`decisionrate` parses but is ignored: no NPC think-rate field exists anywhere
in the game's reflected fields.

### Foliage wind

Grass and plants are static in the vanilla game. SWSE bends them by rewriting
the engine's ARB vertex programs at runtime. **Needs the `foliage` feature.**

Two effects: a constant gentle sway, and foliage that **parts around the
player** as they walk through it - wider while sprinting.

| Command | What it does |
|---|---|
| `wind on\|off` | enable the effect |
| `wind <strength> [speed]` | bend per unit of plant height (0.06 = 6%) |
| `wind weight <n>` | stiffness - tall plants stop whipping (soft curve) |
| `wind push <amt> [radius] [maxheight]` | how hard foliage is shoved aside, and how close |
| `wind axis y\|z` | which local axis is up (**measured: z**) |
| `wind seed on\|off` | per-plant phase, so plants do not sway in step |
| `wind gate on\|off` | restrict wind to foliage draws |
| `wind test on\|off` | exaggerate, for diagnosing direction and pivot |
| `wind dump` | write the live injected programs to `bin\swse_wind_dump.txt` |
| `wind save` | persist to `wind.txt`: every setting, the file's `#` notes kept, written beside it and swapped in |
| `foliage on` | install the bind tracker wind depends on |
| `foliage scan` / `scanned` | capture one frame's textures to a file |
| `foliage progs` | which vertex programs draw foliage |
| `foliage reload` | re-read `foliage.txt` without restarting |

Settings in `SWSEMods\SWSE Wind\wind.txt`, read when the foliage feature
starts - at launch with `foliage = on` - so the effect comes back on by itself
when the file says `enabled 1`. Shipped values: strength 0.075, weight 0.85,
push 0.4 over radius 4.5 (widening toward 7.5 at a full sprint), Z-up,
per-plant phase on, foliage-only gate on. Without the file, `wind on` uses
the same values.

Plants are recognised by fingerprinting textures **as they upload**, in the
same upload hook HD replacement uses. 1.0.x only installed that hook for the
graphics or HD features, so foliage on its own never identified a single plant;
the foliage feature installs it itself in 1.1.

**Which plants move** is data, not code. `foliage.txt` holds texture
fingerprints, 63 of them, and every enabled mod's `foliage.txt` is added in.
An entry may carry:

| Flag | Effect |
|---|---|
| `nopush` | sways, but is not shoved aside by the player |
| `sway=<0..1>` | scales this entry's sway; `sway=0.10` is a 90% reduction |
| `nowind` | shorthand for `sway=0` - completely still |

The 20 trees and canopies carry `nopush sway=0.10`. Trees need their own scale
because the global `weight` control damps by plant *height*, and no height cut
separates a tree from tall grass - one that stilled the trees also killed the
grass. A canopy that visibly travels reads as rubber rather than wood.

The entries were copied from `all_alpha_textures.txt`, a list of every
alpha-cutout texture in the game with its fingerprint (not part of the
release). Cacti, trunks, terrain grass and tyres are excluded entirely. After
editing, `foliage reload` applies it immediately, rebuilding the flags for
textures already loaded.

It survives level changes: the engine recreates its shader programs on load,
which silently reverts the injection, so SWSE reads its own marker back every
two seconds and re-injects. Wind is never injected into a skinned character
program, so character models cannot be affected. Measured cost: **no fps
change**.

### Shader reconnaissance

`shaderdump [file|stats]` dumps every shader the driver holds - GLSL programs
and ARB assembly programs - to `bin\swse_shaders.txt`, and reports how many
vertex programs use fixed-function matrix state. That number decided how wind
had to be built. Read-only; nothing is patched.

### Screenshots

`snap [file.tga]` captures from inside the engine via `glReadPixels`, so it
works regardless of window focus or overlay state. The path may contain
spaces, with or without quotes; without one it writes `bin\swse_snap.tga`.

---

## Self-test - is everything actually working?

`selftest` checks the frame hook, hit reactions, foliage, wind (and its
character-mesh guard), HD textures, graphics, the game build and the level
watcher, then each of AI tuning, triggers, character tuning, player tuning,
prefs edits and ray tracing that is switched on, and reports what it found. It
runs automatically once per level, 5 seconds after the level watcher sees the
player (in safe mode once, 10 seconds after the first frame - below), and
writes to `bin\swse_selftest.txt` as well as the console.

With the 1.1 defaults it reads:

```
=== SWSE self-test ===
[PASS] frame hook     last frame 16 ms, worst 627 ms, 3 stalls >80ms
[PASS] game build     Steam HD (link stamp 508AA980, image 006BB000, entry 002E8CA8)
[PASS] level watch    level up (epoch 1, 5 s)
[INFO] agentdebug     off
[OFF ] features       hitreact, foliage (and wind), hdtextures, graphics, aituning, triggers, npctuning, playertune, prefsedit, raytrace  (`features <name> on` switches one on)
=== 3 passed, 0 warned, 0 FAILED ===
```

and with those systems switched on, for example:

```
[PASS] hit reacts     ON, actors present, 301 polls, 0 hits seen
[PASS] foliage        63 fingerprints, 11 live here, 216 binds/frame
[PASS] wind           ON, 3 programs injected, 0 failed
[PASS] HD textures    981 installed, 213 substituted so far
[PASS] graphics       post-process pipeline ready
```

**Every check asserts on evidence of work done, never on a flag.** This exists
because a change to the actor scan once left hit reactions completely dead
while the feature still reported itself ON: the hook was installed, the flag
was true, and it detected nothing because its actor list was empty. So
"enabled" is not a pass - "enabled, has actors, and is polling them" is. A
subsystem that is on but idle reads as FAIL, because that is precisely the
state that went unnoticed.

`WARN` means genuinely not applicable here (no foliage in this level, no HD
texture encountered yet, a tuning file that asks for nothing, an unrecognised
game build) and never means "working". A system that is switched off is not
checked at all: it is named on the `[OFF ]` line. Beyond the one-line checks,
`difficulty`, `triggers`, `tuning`, `playertune` and `prefs status` report on
them.

**In safe mode** (an unknown game build) there is no level watcher to time
it: it runs once, 10 seconds after the first frame. It heads its report
`unknown build: safe mode`, checks what runs there, and names the systems that
read or call the game on a `[SAFE]` line instead of failing them:

```
=== SWSE self-test - unknown build: safe mode ===
[PASS] frame hook     last frame 16 ms, worst 627 ms, 3 stalls >80ms
[WARN] game build     unknown build: safe mode - the checks below are the systems that run without the game (`status` names the exe)
[INFO] agentdebug     off
[SAFE] unknown build  hitreact, level watch, aituning, triggers, npctuning, playertune, prefsedit - they read or call the game, so they do not run on this build
[OFF ] features       foliage (and wind), hdtextures, graphics, raytrace  (`features <name> on` switches one on)
=== 1 passed, 1 warned, 0 FAILED ===
```

**Plugins (1.1)** follow SWSE's own lines. A plugin that is on runs its own
checks if it registered any (counted in the summary like SWSE's); otherwise
SWSE's evidence is that it has been called at all since it was switched on. A
plugin that faulted this session reads FAIL; one that is off joins the
`[OFF ]` line:

```
[PASS] hello          plugin on: 1019 call(s), 0.00 ms/frame (no self-test of its own)
[PASS] qagl           94332 binds and 215 uploads seen
[FAIL] qagl2          FAULTED this session: exception C0000005 in a bind listener at qagl2.dll+0x128C
```

---

## Additive hit reactions

Shooting a character pushes the bone that was actually hit. The reaction is
**additive**: it rotates bones on top of whatever animation is playing, rather
than replacing it, so the character keeps walking, aiming or reloading while
absorbing the hit. **Needs the `hitreact` feature.**

### The pipeline

1. **Detect the hit.** A health-drop watcher sweeps live actors, reading
   current/max health at `actor+0x78`. A drop means damage; the amount is the
   delta.
2. **Find where.** Bolts are tracked in flight (hook at RVA `0x91180`,
   position at `+0x24`). The nearest bolt within 12 units of the victim in the
   last moment gives the impact point. With no bolt - melee, explosions - it
   falls back to the torso.
3. **Resolve the bone.** The impact point is matched against the live Granny
   pose to find the nearest bone. Resolve rate is 100%, accurate to 0.1-0.7
   units.
4. **Promote to something visible** (see below).
5. **Spread, envelope, accumulate, write once.**

### Intensity: damage → angle

Damage is converted to a **fraction of the victim's max health** before it
reaches the curve:

```
dmgUnit = 100 * damage / maxHealth        (ratio mode, default ON)
scale   = clamp(curveFloor + dmgUnit * curvePerDmg, 0, curveMax)
angle   = strength * scale                 radians at peak
```

| parameter | default | meaning |
|---|---|---|
| `curveFloor` | 0.25 | even a scratch registers |
| `curvePerDmg` | 0.04 | per 1% of max health lost |
| `curveMax` | 0.50 | ceiling, so a killing blow cannot snap the rig |
| `strength` | 1.2 rad | angle at full scale |
| duration | 360 ms | |

**Why the ratio matters.** With an absolute curve, Wolvarks looked immune - a
1.4-damage hit produced 5.4°, while the same weapon on the player read fine.
Health totals differ by orders of magnitude across characters (Clakkerz sit at
100,000), so a percentage makes "a scratch" and "nearly killed me" mean the
same thing on every rig, including ones never tested. `hitreact ratio 0` uses
raw damage instead.

### The envelope: how a reaction moves over time

The original curve was `(1-t)²` - full rotation on the very first frame, then a
glide back. That **popped on** and read as a glitch. A real impact has a fast
but finite onset, overshoots, and settles, so the envelope has two phases:

```
attack   t < 0.18 :  k = smoothstep(t/0.18) * strength      (3u² - 2u³)
settle   otherwise:  u = (t - 0.18) / 0.82
                     k = ((1-u)² - overshoot * sin(pi*u)) * strength
```

`attack` 0.18 (fraction of duration spent rising), `overshoot` 0.12 (how far
past rest it counter-swings). Smoothstep avoids a corner at either end;
`attack 0` restores the instant onset. An expiring reaction runs one final pass
at `k = 0` so nothing is left in the pose.

### Spread along the spine

A hit does not rotate one bone. It spreads **up the spine away from the root**
across `chain` links, each taking `falloff` of the previous - chest full, neck
less, head least. Defaults: **3 links, 0.6 falloff**. Weights are fixed per
link so the unwind cancels exactly.

Chaining the other way, toward the root, reached the pelvis and blew the hips
out, because rotating a bone that low drags the legs with it.

### Limb-mass promotion: why hits used to be invisible

Reactions on outlaws read as "barely noticeable" while the numbers looked
healthy - 19.5° rotations were landing on **fingertips**. A rotation is only
visible if geometry hangs below it.

So the struck bone is promoted toward the root until at least **`limbmass` (14)
descendants** hang below it, capped at 8 steps. The count comes from the
skeleton's subtree sizes, derived at runtime - no per-rig data is hardcoded.

### Concurrency: surviving a burst

Accumulating rather than multiplying stops reactions compounding, but ten
simultaneous hits still **summed** to an absurd angle and tore the pose apart.
When more than one reaction is live on a character, each accumulated
quaternion's **angle** (not its components) is scaled by `1/N`, floored at
**0.45** so the last hits stay readable. Borrowed from the Fallout NV additive
hit reaction mod, which divides blend weight the same way.

### Arm damping

`armdamp` counter-rotates the arm roots by a fraction of what was applied to
their parent, so the torso absorbs the hit while the hands - and the crossbow
they hold - stay pointing where they were. `0` = arms ride the body (default,
signed off in play), `1` = arms hold station.

Detecting arm roots requires **exactly 2 branches** off the chest: a 20-bone
creature rig had four 3-link chains and three were mistaken for arms.

### Commands

| Command | What it does |
|---|---|
| `hitreact on\|off\|test` | enable / fire a test reaction |
| `hitreact <strength> <ms>` | peak angle in radians and duration |
| `hitreact curve <floor> <perDmg> <max>` | the damage→scale curve |
| `hitreact ratio 0\|1` | damage as % of max health, or raw |
| `hitreact ease <attack> <overshoot>` | envelope shape |
| `hitreact chain <links> <falloff>` | spread along the spine |
| `hitreact limbmass <n>` | minimum descendants before a bone is used |
| `hitreact armdamp <0..1>` | how much the arms resist the torso |
| `hitreact save` | persist to `hitreact.txt` |
| `hitreact hits` | recent hits with damage and resulting scale |
| `hitreact bones` | which bones the last reaction actually wrote |
| `hitreact bolts` | tracked projectiles |
| `hitreact freeze` / `tpose` | diagnostics: stop animation / flatten to bind pose |

### Switching it on

Settings live in `SWSEMods\SWSE Combat\hitreact.txt` (the last enabled mod that
provides one wins). With the `hitreact` feature on, the file is read at launch,
and `enabled 1` installs the pose hook, the bolt hook and the health watcher and
switches the reactions on. With no file the compiled defaults are used and it
still comes up on - those defaults ARE the values signed off in play:

```
enabled 1
strength 1.2000
ms 360
curve 0.2500 0.0400 0.5000
ratio 1
ease 0.1800 0.1200
chain 3 0.6000
limbmass 14
armdamp 0.0000
```

### Safety

`ApplyReactions` wraps its body in `__try` and disables itself on a fault
rather than taking the game down. Turning it off - `hitreact off` or
`features hitreact off` - never unpatches the hook (the render thread may be
inside it - that crashed the game once); it just stops contributing. Cost is
about **2 fps**.

---

## Player, movement and items

**Player:** `hp`, `stam`, `sethealth`, `heal`, `maxhealth`, `maxstamina`,
`god`, `kill`, `pfield`, `playertune`

**Movement:** `pos`, `yaw`, `savepos`, `tp`, `tpxyz`, `up`, `move`,
`gravity`, `aircontrol`, `jump`, `speed` - see
[Positions and facing](#positions-and-facing)

**Items:** `grant`, `giveartifact`, `allartifacts`, `artifacts` (all 53),
`giveweapon`, `moolah`, `money`, `ammo`, `defaultammo`, `noammo`, `crossbow`,
`noweapons`, `artifact`

**View / state:** `fps` / `nofps` / `sniper`, `steef`, `stranger`, `naked`,
`save`, `checkpoint`, `loadsave`, `healthbars`, `weaponhud`, `tphome`,
`tpreset`

**World:** `warp <level|0-6>`, `levels`

**Menu and input:** `key`, `menu`, `newgame`, `continue`, `skipcut`,
`inputst` (which input paths the game reads; its second line, `guards:`,
counts the input the console kept from the game and background mode's
keyboard hand-backs)

**Menus without keys (1.1).** `menu` drives the game's Flash menus the way
their buttons do: it calls the game's own fscommand handler with the screen's
command, so nothing is typed and it works with the game behind other windows.
- `menu list`: the live Flash movies, and which screens `menu fs` can drive.
- `menu continue`: the main menu's CONTINUE, which loads the last save.
- `menu skip` / `menu resume`: a paused movie's SKIP / PLAY.
- `menu fs <screen> <cmd> [arg]`: any screen's own command, for example
  `menu fs Pause quick_save` or `menu fs LoadGame selected_slot 2`.
  Screens: MainMenu, LoadGame, SaveGame, NewGame, Pause, movie, Difficulty,
  Options, Controls, Sound, Extras, LevelSelect, EnterName, MovieView,
  Collectables. The command names are the SWF's own; see
  `swse/research/RE_UI_AND_ICONS.md`.
- `menu <n>`: the old key-press form, n x Down then Enter.

`continue` uses `menu continue` when the main menu is up, and key presses
otherwise.

**Music:** `combatmusic`, `tensionmusic`, `popmusic`, `pushmusic`,
`transmusic`

---

## Free camera - `freecam` (1.1)

The developers' fly camera is still in the HD game, controls and all; only the
key that switched it on was removed. `freecam` switches it on.

| Command | What it does |
|---|---|
| `freecam` (or `freecam status`) | on or off, and the camera's position, yaw, pitch and speed |
| `freecam on` / `freecam off` | detach the view / return to the player camera exactly as it was |
| `freecam toggle` | on if off, off if on - for a key: `bind F6 freecam toggle` |
| `freecam pos <x> <y> <z>` | put the camera there (switches freecam on) |
| `freecam look <yaw> [pitch]` | aim it, in degrees. Yaw is the same angle `yaw` and `pos` report for Stranger; pitch is up from the horizon (default: keep the current one). Switches freecam on |
| `freecam speed [n]` | flying speed at full stick, in world units per second (default 40) |
| `freecam sens [n] [updown]` | how far the mouse turns the view, in degrees per 100 mouse counts, times the game's own mouse sensitivity (default 8). A second value sets up/down on its own; a negative one inverts it. `freecam sens` shows what is in use |

- **Flying.** The camera reads the game's own controls:
  - movement (WASD or the left stick) flies;
  - the right trigger rises and the left trigger sinks;
  - clicking the left stick speeds up and clicking the right stick slows
    down.

  Which PC keys act as the triggers is still to be checked.
- **Turning.**
  - The mouse turns the view: right turns right, forward looks up. SWSE turns
    the camera by the mouse's own motion, because the developers' camera,
    older than the PC version, reads the mouse too weakly to turn by it.
  - The arrow keys turn it too, for anyone without a mouse.
  - A gamepad's right stick turns it as the developers wrote it.
- **Stranger stays put.** While the view flies, the game skips Stranger's own
  input handling, as the developers' build did while its fly camera was up.
  He does not walk, jump, turn or fire. If a movement key is held when
  freecam starts, he keeps his controls until you let go, so he is not left
  running.
- **The console.** While the console is open nothing reaches the game, so the
  camera holds still while you type. Close it to fly.
- **Tools and background mode.** With the game in the background it gets no
  real input, and the camera stays exactly where `pos` and `look` put it.
  `pos` and `look` take effect at once, even while the game is paused.
- **Switching itself off.** A level change or load, a death, or a cutscene
  turns freecam off, with the reason in the console.
- **Far away.** Parts of the level far from Stranger may not be loaded. A
  `pos` far away through walls may leave some rooms undrawn; move in steps.
- **Game builds.** Refused on a game build SWSE's addresses were not measured
  on.

How it works, with the evidence for every address: `swse/research/FREECAM.md`.

---

## NPCs

The largest command group, and the one with the most reverse-engineering
behind it.

**Spawning / placement:** `spawn`, `spawnhere`, `npcnow`,
`npcdupe`, `npccount`, `dupetype`, `npcreplay`, `npchere`, `npclast`,
`spawnclone`, `npcspawn`, `bring`, `sendnpc`, `spawnradius`, `critters`,
`spawnat`, `reserve`. These are research tools: adding a new, working NPC is
not supported in this version. `spawnnpc` is retired and refuses: it moved a
live piece of the level to you and wrote the wrong fields. NPCs SWSE builds
itself (`npcnow` and the replay commands) are not drawn and do not act, and
NPCs moved across the level (`bring`, `sendnpc`, `spawnat`) arrive without
their AI running.

**Tuning:** `npchealth`, `npcelite` (promote a fraction to elites), `npcgib`,
`npchurt`, `npcaff`, `allnpcs`, `ai`, `types`, `tuning`

**Finding things:** `npcs`, `npctypes`, `npctags`, `spawntypes`, `npcnear`,
`whereis`, `geominst`, `resolve`, `strhash`

**Hostility and alarms:** `townpanic`, `raid`, `raidmode`, `attack`, `decoy`,
`feud`, `findtarget`, `scantargets` - research tools; none of them makes NPCs
fight each other (see the known limit below).

**Events:** `triggers [list|reload|test <name>|on|off]` - mod-defined events
from every enabled mod's `triggers.txt`. Needs the `triggers` feature to
fire on its own.

### Play as a character - `playnpc` (1.1)

| Command | What it does |
|---|---|
| `playnpc` (or `playnpc list`) | the level's character types, and each one's state: `ready` (in memory now), `load` (can be streamed in first), `far` (only comes with its own area), `nobody` (no body in this level's data) |
| `playnpc <name> [nospin] [bare] [scale <x>]` | become that character: its body and its own animations. Stranger spins into it unless `nospin`. `bare` leaves its hat or weapon off; `scale` draws it from 0.3 to 1.9 times its size |
| `playnpc off [nospin]` | back to Stranger (spinning back unless `nospin`) |
| `playnpc status` / `playnpc states` | what is installed, and which data blocks are held |
| `playnpc anims` | QA: one sample of the animation blend (weights, and each clip's weight, speed and clock) in swse_log.txt |
| `playnpc runtest [ms] [sprint] [buck]` / `runtest stop` | QA: hold W (and the right mouse button with `sprint`; a left click at 0.8 s with `buck`) for `ms` and sample every 250 ms |
| `playnpc hurt <n> [stamina]` | QA: take n off the current health (or stamina), for checking how much comes back with you |
| `playnpc textures [name]` | QA: each material of the body and of what it wears - where its texture lives, whether that is loaded, and what it draws with (log too) |

- **Loading.** A character that is not in memory is streamed in first: the
  console says "loading ...", and each data block's arrival is logged. It
  took 31-125 ms in the live tests. After 20 s it gives up and names the
  block that never came.
- **Staying loaded.** While you play a character, SWSE marks its data as
  needed every frame, the way a live character of that type does. Walking
  away from its area therefore does not turn you back. If the game unloads it
  anyway, you are put back as Stranger with a message.
- **Movement.** The gaits a character lacks (walk, trot, canter, run,
  turnarounds, skids, bursts, landings, jumps) are filled from its own clips,
  so it never plays a Stranger animation. Its speed is its own: root motion
  from its clips. At full speed in Act 1 a guard keeps its fastest clip playing
  where the engine's gait blend would have broken the pose.
- **Health and stamina** are the character's own. Townsfolk, natives and the
  other characters the game never lets die are capped at Stranger's. You come
  back to Stranger, or move on to the next character, at the same fraction.
  `playnpc status` shows both.
- **What it wears**: hats, quivers, and its weapon (the outlaw mortar carries
  its mortar). `bare` leaves them off.
- **Textures.** The body and everything it wears show their real textures.
  A piece can borrow its texture from another character's data (boilzbooty's
  shotgun, the outlaw shooter's hat); SWSE loads and keeps that data too, as
  the game does for its own characters, before the character goes on.
- **Death** plays the character's own death animation; the checkpoint reload
  brings Stranger back.
- **Limits** while playing a character:
  - no double jump;
  - no first-person or sniper view;
  - no Stranger attacks, whatever the camera does: the crossbow (firing,
    reloading, its punch and switching ammo), the buck and the headbutt (out
    of stamina too), and ramming are all off (the character's own melee and
    weapon are coming). The sprint stays;
  - no manual Stranger/Steef switch;
  - no foot IK.
- **Scale.** The Giant Sleg is drawn at 1.9x, because the PC port's bone
  packing wraps at 2x.
- **Endings.** A level change, a death or a checkpoint reload returns you to
  normal. The body is not saved.
- **Game builds.** Refused on a game build SWSE's addresses were not measured
  on.

The design, the engine's streaming rules it relies on, and every live test
are in `swse/research/PLAYNPC.md`.

### Character tuning - `characters.txt`, `console.txt`, `npctuning`

`characters.txt` gives per-character health, gib and hurt reaction, applied as
each level spawns them:

```
<hash>    <health>  <gib>  <hurtReaction>     ("-" = leave stock)
*         -         -      -                  (every other character)
```

`console.txt` holds named rules that work by measuring rather than naming:
`noimmortals = 100` gives every character the game shipped as unkillable
(100000 hp) that much health instead, and `immortalsgib = on` lets them gib.

With the `npctuning` feature on, both are read at launch and applied as each
level spawns its characters; `tuning` loads them by hand for the session, and
`mods reload` re-reads them while the feature is on. In 1.1 every enabled
mod's `characters.txt` is added in (up to 64 rules), as the mod registry always
documented, and `console.txt` is the last enabled mod's. The shipped files hold
their rules commented out, as examples: live, they made the game's 22 protected
characters - townsfolk, natives, farmers, storekeepers, the Vykker Doc - mortal
for anyone who switched `npctuning` on for rules of their own.

A health rule writes `m_health` only. 1.0.x also wrote the next field believing
it was a maximum; the reflection says it is `m_stamina`, so every health change
from `npchealth`, `allnpcs` or `characters.txt` also set that character's
stamina.

`ai <typehash> [field value]` reads and writes one character type's AI and
weapon values (`firerate`, `reload`, `reloadmax`, `accuracy`, `misstime`,
`6thsense`, `seedist`, `hidevolsee`, `sightcombat`, `relax`) in the game's own
units - times in seconds. `ai <typehash>` on its own shows them all. In 1.1 it
writes the AIPrefs object embedded in the character's `NPCPrefs` and the
weapon object the character really uses, found through the resource registry;
1.0.x wrote into the AIPrefs vtable and through the weapon's hash as if it were
an address, with AI offsets four bytes short.

**Known limit:** NPCs fighting each other is not supported in this version.
The game's own attack rules make characters hostile to the player only: its
attitude check, its melee hit filter and its bolt-collision check allow an
attack only when the target is the Stranger (or Steef). The commands above
can retarget NPCs or (`feud`, untested) inject damage, but they do not make
characters fight. `m_affGenerally`, the field `npcaff` writes, is an
ammo-immunity rule, not an affiliation: use `npcaff` with care.

---

## AgentDebugMode

`agentdebug [on|off]` (alias `background`) keeps the game simulating while
alt-tabbed **and** gives up the cursor and keyboard, so the desktop stays
usable. Off by default, and every hook involved is installed but inert, so
there is no behaviour to regress.

The first version only did the first half and trapped the pointer inside the
game window. Real focus is now tracked separately from the focus the engine is
told about.

**Background mode from launch (1.1).** Start the game with the environment
variable `SWSE_AGENTDEBUG=1`. SWSE switches AgentDebugMode on at the first
frame and mutes the game's own audio session. The game then runs unattended
behind other windows, and the mailbox keeps working. This is how tools and
agents drive it without taking over the desktop.
- **Park the window behind others; never minimize it.** A minimized game's
  main thread stops handling window messages, and the game stalls.
- **Key presses a tool injects are frame-safe.** A press is held for whole
  game frames, so it is seen even at a low frame rate. `continue` loads the
  last save this way.
- **It keeps your keyboard.** The game used to come to the front after a level
  load while you typed in another app, and take your keys. Now, while
  AgentDebugMode is on, if the game comes to the front without a click on its
  window or its taskbar button, SWSE hands the foreground straight back to the
  window you were in, and logs a line starting `background: the game came to
  the front without a click`. A click on the game or its taskbar button still
  hands it over, and so does switching to it from the shell (Alt+Tab, Task
  View, the Start menu). Every activation in this mode is logged with what was
  decided. `inputst` counts the hand-backs on its `guards:` line.
- **The cursor.** The game hides its cursor with a loop that calls
  `ShowCursor(FALSE)` until the count goes negative. In the background, SWSE
  answers with a counter of its own, so the loop ends. The real count is put
  right when the window gets focus back.
- **`mute [on|off]`** switches the per-app mute (the game's entry in the
  Windows volume mixer) at any time. `mute off` brings the sound back.

---

## What we learned from Stranger: Armed to the Teeth (AT3)

**Stranger: Armed to the Teeth** - AT3, by **Racewizard** - is a mod toolkit
built on SWSE. It edits the game's serialized records on disk; SWSE 1.1 was
built partly to support it, and lining its findings up against SWSE's live
view of the same data taught us the following. Each item is verified live in
the game or by disassembly. Thank you, Racewizard.

* **Player facing is not a player or motion field.** It is the rotation of the
  player's geometry world frame: `G = *(player+0x14)`, a 3×3 rotation at
  `G+0x30` (rows m0-m2 / m3-m5 / m6-m8), foot position at `G+0x54`.
  `GetFacing` returns `-(m1, m4, m7)`; Z is up and the model's forward is local
  -Y. The engine's setter `Actor::SetFacing` (RVA `0x1FC80`, vtable `+0x104`,
  `__thiscall(this, float dir[3])`, `ret 4`) is a pure store that nothing
  rewrites per frame. Live: `yaw 90` / `yaw 270` visibly turn Stranger 180°
  and the facing persists. SWSE reports the yaw as that matrix's rotation
  angle, `atan2(m3, m0)` - the angle AT3 reads from level records and builds
  its placements from - so a yaw moves between SWSE, AT3 and a level record
  unchanged. → `swse/research/PLAYER_FACING.md`
* **Prefs objects carry their own path hash at `+0x0C`** - verified live on
  `NPCPrefs`, `NPCWeaponPrefs` and `BoltDamagePrefs`. That is how the prefs
  editor finds any loaded record from its path.
* **Ammo types are prefs records** at `/data/prefs/weapons/<name>.txt` (path
  hash from the game's own hasher: `damagearmadillo` = `7AE0C662`,
  `damagedynamite` = `1B2A36F7`) whose live class is `BoltDamagePrefs` (or a
  sibling - all derive from `BoltPrefs`): `m_maxKnockSpeed` at runtime
  `+0x140`, `m_maxKnockSpeedPlayer` at `+0x144`. AT3 found them in the
  **serialized** records at record+164 / +168 - serialized and runtime layouts
  differ. All 31 vanilla values read live match AT3's `AmmoKnockback.csv`:
  thudslug 15, riotslug 20, boombat 20, sniper wasp 20, bola blast 15, super
  stingbee 0.1, rabid pounce 8, the rest 0; player `-1` everywhere. The
  engine's knock getter (`0x4A1490`) uses the player value when it is 0 or
  more and `m_maxKnockSpeed` otherwise, so `-1` means "the same as for NPCs"
  and `0` "never knocks the player".
* **`FIELD_OFFSETS.tsv`'s class attribution was noisy.** It filed
  `m_maxKnockSpeed` / `m_maxKnockSpeedPlayer` under `BoltSurfaceSndPrefs`; the
  live class is `BoltDamagePrefs` (the offsets were right). It has been
  replaced: `swse/research/REFLECT_FIELDS.tsv` - 5,697 fields in 191
  classes, extracted by emulating the game's own reflection initialisers
  (`tools/reflect/`), with exact classes, types and inherited fields - now
  generates the prefs editor's table. → `swse/research/PREFS_EDITOR.md`
* **`m_missTime` (`NPCWeaponPrefs +0x1AC`) is stored in seconds**: 0.010
  (10 ms) on almost every NPC weapon (live, `weapons`). AT3 found that pushing
  it very high makes enemies nearly unable to land a hit - which is why SWSE
  now applies it: `misstime` in `aiprefs.txt`, in milliseconds, 10 = vanilla.
* **The game's path hash** (RVA `0x24D920`) is a reflected CRC-32 (polynomial
  `0xEDB88320`, init `0xFFFFFFFF`, no final xor) over the path upper-cased with
  `/` turned into `\`, with the low byte of the path length folded in last.
  AT3's `ModTools/gamehash.py` documents it for offline use; SWSE uses the
  in-process hasher (`strhash <path>`).
* **`.lvl` object records** (documented by AT3): a chain of records, each
  starting with the token `0x7A60600D` and an absolute pointer to the next,
  the class-hash marker `0x000B4265`, a zone index at `+20`, a 3×3 rotation at
  `+25`, the translation at `+61` and a uniform scale at `+73` ("+73 is a
  scale, not a sentinel"). A placed object only appears if its zone index
  matches where it stands and its geometry's zone bundle is resident.
  Reference only for SWSE today - it is an offline format.
* **Latent 1.0.x bugs, found while testing 1.1 for AT3**: foliage wind never
  identified plants unless graphics or HD textures had installed the
  texture-upload hook; AI tuning's automatic apply, triggers and the self-test
  depended on hit reactions being on; `tuning` read a `settings.txt` that no
  longer shipped, so `console.txt`'s `noimmortals` was never read; `up` moved
  the player along Y (sideways) instead of Z; the gold debug square was drawn
  whenever graphics was off. All fixed in 1.1, with the others listed in
  [CHANGELOG.md](CHANGELOG.md).

### Mapped but not yet run: the AT3 discoveries catalogue

`AT3_DISCOVERIES.md` (in the development repository's research notes) maps
everything else AT3 found onto live offsets and proposes commands for it. It is
static analysis of the exe and of retail records - **nothing in it has been
run in the game yet** - so read it as a map, not a feature list. Its headline
results:

* A prefs record's serialized order is its ParamIO array order, and its runtime
  layout is the member offsets in the same descriptors, so every AT3 `ser+N`
  offset converts to a live one. `REFLECT_FIELDS.tsv`'s dotted names
  (`m_spAIPrefs.m_sightAgit.m_seeDistance`) can be used directly in `prefs` and
  `prefs.txt`.
* `NPCPrefs` embeds three sub-prefs objects - ActorPrefs at `+0x10`,
  MotionPrefs at `+0x8C`, AIPrefs at `+0x118` - so a character's sight, its 42
  named combat tactics (AttackParams) and its body physics are all reachable
  from its NPCPrefs.
* Every prefs or tag record describes itself as
  `[0x000B4265][class-name hash][params]`, so AT3's magic numbers are class ids
  (NPCTag `0616073E`, InstancedObjectTag `2B9F6678`...).
* The engine **does** create NPCs at runtime - when a character gibs
  (`m_onGibSpawnNPC`: the Shock Tank becomes a wolvark shooter) and to refill
  spawn pools - which `NPC_SPAWNING.md` said never happens. Later research
  found why SWSE's constructed NPCs never render (the game files them under
  no zone) and that the game itself spawns by releasing characters it placed
  in a level's spawner pools at load. Spawning through SWSE is not supported
  in 1.1.1.
* `m_affGenerally` / `m_affList` read as an **ammo-immunity rule**, not an
  affiliation: `npcaff <type> 0` may make a type immune to all player ammo.
  `m_species` (outlaw, wolvark and slog hostile; native and townsfolk not)
  sets how a character treats the player; later research found the game's
  attack checks allow attacks on the Stranger only, so it does not on its own
  make NPCs fight each other.
* Characters drop loot through spawner slots (any character could drop ammo or
  moolah), each zone carries its own fog and ambience block, and the weapon
  damage, clip, area and homing fields are named.

Its top ten proposed commands, by value for effort: `npcgibspawn` leading to
`npcspawn`, a zone and height fix for `npcnow`, per-type `npcai` sight and
tactics, `npcspecies`, `npcammo` (replacing `npcaff`), `npcloot` with `spray`,
`wpn`, `fog` / `env` / `hunters`, `npcgibfx`, and a key=value `characters.txt`
applied in the spawn hook. Of these only `wpn` exists (1.1, see
[Characters and weapons by name](#characters-and-weapons-by-name---npc-wpn-11));
the console's existing `npcspawn` replays a captured spawn and is not the
proposed command.

How AT3 and SWSE 1.1 fit together - files, commands, and the one behaviour
change it needs to know about - is in [AT3_INTEGRATION.md](AT3_INTEGRATION.md).

---

## Extending the AI tuning - technical reference

This section is for anyone who wants to add a new tunable field, or reach a
class the tuning does not touch yet. It is the part that took longest to get
right, and most of the cost was avoidable. (For a one-off change to any loaded
prefs record, the prefs editor above needs no code at all.)

### The engine's reflection system

`stranger.exe` registers its reflected fields at startup - `reflect_dump.py`
finds **1910** of them this way; the exact extraction below finds more. Each
one is a 12-byte descriptor built inline, and - this is the useful part -
**the field's offset is a literal in the instruction stream**:

```
6a 0c                 push 0Ch                 ; sizeof(descriptor)
e8 <rel32>            call operator new
c7 06 <typedesc>      mov  [esi],   <type>
b8 <nameVA>           mov  eax,     <field name string>
c7 46 08 <offset>     mov  [esi+8], <FIELD OFFSET>     <-- exact, not inferred
```

`tools/reflect_dump.py` reads this:

```bash
python tools/reflect_dump.py                      # summary
python tools/reflect_dump.py --field m_fireRate   # locate one field
python tools/reflect_dump.py --csv out.csv        # all 1910
```

**The exact table (1.1).** `tools/reflect/extract2.py` goes further: it
emulates the game's own reflection initialisers and reads each class's field
set through its vtable, which gives the exact class, type and inheritance of
every field - `swse/research/REFLECT_FIELDS.tsv`, 5,697 fields in 191
classes, inherited fields listed under every class and embedded structs dotted
(`m_spAIPrefs.m_sightCombat.m_seeDistance`). `tools/gen_reflect_header.py`
turns it into `swse/reflect_gen.h`, the name → offset table the prefs editor
uses.

**Do not use `swse/research/REFLECTION_SCHEMA.md` for addressing memory.**
It says so in its own header - its class grouping is a heuristic based on name
proximity, and it is wrong in ways that look right. It filed the perception
fields under a class called `CoverDuration`, which has nothing to do with
sight.

**Class names come from the extraction, not from offset order.**
`reflect_dump.py` splits classes where offsets decrease - a class registers its
fields in ascending order, so a decrease is a boundary - which took the dump
from 49 merged blobs to 294 runs and produced `FIELD_OFFSETS.tsv`. Its offsets
are right, but the exact extraction showed the right class on only 689 of its
1,707 rows: neighbouring classes run on without a decrease. Use
`REFLECT_FIELDS.tsv` for class names.

### Finding a prefs object at runtime

The obvious route does not work. **1.0.x's `ResolvePrefs()` called RVA
`0x23880`, which is `GetPrefs<NPCPrefs>`**: handed any hash that is not a
loaded NPCPrefs - every weapon and AI-prefs hash - it constructs a default
NPCPrefs carrying that hash, registers it and leaks it, so the offsets got
applied to that fresh object and read as plausible garbage (direction vectors,
`0.707` = cos 45°). 1.1 only falls back to it if the registry below cannot be
read.

Routes that do work, in order of preference:

**1. The game's resource registry.** `ResourceManager*` at RVA `0x5D55A8`
registers every loaded resource by its path hash (`+0x0C`). In a running game
that is a vector at `+0x30` - `{data, capacity, count}` of resource pointers
sorted by their `+0x0C` (6,416 entries in `lm_level_01`, measured) - so a
binary search of about 13 reads, with no call, no reference count touched and
nothing created, answers "the loaded record for this path" for any class. (A
hash map at `+0x2C` is the form the constructor sets up; it is empty while the
game runs and is only a fallback.) The prefs editor, `ai` and the character
tuning use it.

**2. By its own path hash.** Every prefs object carries its own path hash at
`+0x0C`, so a heap scan for the hash, keeping only hits with a valid RTTI
vtable 12 bytes earlier, finds any loaded record by path. The prefs editor's
fallback.

**3. By vtable, recovered from RTTI.** This is real identification and is safe
to write through.

```
.?AVNPCWeaponPrefs@@   ->  vtable 0x776454   (RVA 0x376454)
.?AVNPCPrefs@@         ->  vtable 0x767E1C   (RVA 0x367E1C)
```

The walk is: find the type descriptor (the mangled name string, minus 8 bytes),
find the `RTTICompleteObjectLocator` that points at it, then find the vtable
whose `[-1]` slot points at that locator. `stranger.exe` is ASLR'd, so rebase:
`GetModuleHandle(NULL) + RVA`. **The method self-checks** - it returns
`NPCPrefs = 0x367E1C`, matching the value hardcoded in `PrefsOfNpc` from
entirely separate work.

**4. By memory shape, when no vtable is available.** Only acceptable with a
*structural* signature, not a plausibility test. The perception object has four
sight blocks at a fixed `0xA4` stride, and across those four states only two
fields ever change:

```
6th sense    10   10   10   10      invariant
seeAbove   1001 1001 1001 1001      invariant, and == seeBelow
h/v angle    90   90   90   90      invariant
instant      10   10   10   10      invariant
------------------------------------------------
seeDist      50  100  100   75      varies   <- the model
hideVol       1    2    3    3      varies   <- the model
```

Requiring all six invariants across all four blocks is a demanding test that
unrelated memory does not reproduce.

> **This is the mistake to learn from.** The first version matched only "four
> blocks of plausible floats". A window of believable floats stays believable
> when shifted four bytes, so it matched overlapping slides of the same object
> *and* unrelated objects. Writing through those corrupted the game - a
> flashing character model, and once a hard crash. The tell was there and
> ignored: the hit count jumped from 7 to 19 between runs. **If a scan's count
> is unstable, stop and fix identification before writing anything.**

### Joining a character to its equipment

Both classes carry their own path hash at `+0x0C`, which gives a clean join:

```
NPCPrefs +0x498  m_rangedWeapon   ==   NPCWeaponPrefs +0x0C  (own hash)
NPCPrefs +0x118  m_spAIPrefs      an EMBEDDED AIPrefs object (vtable RVA 0x377B30)
```

The second line is a 1.1 correction from the exact reflection table: 1.0.x read
`+0x118` as a hash. The embedded AIPrefs object is the very perception object
the shape scan above finds.

`SWSE_NpcGuns()` performs it. This is what makes tuning **selective** - a bare
scan for weapon objects has no idea whose gun it is, but the join gives each
weapon's owner, so the protected cast (100000 health) can be excluded. The
unset sentinel for any hash-valued field is **`0x2DFD1072`**; it does not
resolve and must be special-cased.

### The apply model - baselines, not increments

`aitune.cpp` stores the **shipped value** the first time it touches an object
and computes every write from that baseline:

```c
value = baseline * multiplier;      // never  value = value * multiplier
```

Two properties fall out, and both are worth preserving in any extension:
applying a profile twice does not compound, and restoring lands on the exact
original numbers rather than on `value / multiplier` rounding drift.

A shipped value of `0` means the field is unused for that character. Skip it -
multiplying it writes `0` and looks like it worked.

### Adding a new tunable field

1. `python tools/reflect_dump.py --field m_yourField` - get the exact offset
   and see which run (class) it belongs to.
2. Confirm the class has a recoverable vtable; if not, find a structural
   signature with invariants, not a range test.
3. Read it live first - `peek <addr>`, or `prefs get <target> <field>` - and
   sanity-check the values against what you see in play. **Units are not
   obvious.** `m_fireRate` is shots per second, not a delay. `m_accuracyWidth`
   is a spread, so lower is better. `m_missTime` is seconds. The first two were
   documented backwards here at first.
4. Add the field to `WBase`/`Baseline`, capture it, and scale from the
   baseline.
5. Add the key to `FieldOf()` and document it in `aiprefs.txt`.

### Things that do not exist

Worth knowing before spending time looking:

* **No NPC think-rate field.** Nothing for decision cadence anywhere in the
  reflected fields. `m_checkEverySeconds` belongs to a level-scripting trigger (it
  sits with `m_restrictToGuyType`, `m_restrictToBrainState`,
  `m_dependOnGlobalVar`), not the NPC brain.
* **No per-character animation-rate field.** The only `m_animSpeedLo/Hi`
  belongs to `DestructablePrefs` in the exact table (the 1.0.x notes, working
  from the noisier one, filed it under the ambient critter class).
* **No per-character variety in `m_missTime`** - it ships at 10 ms on nearly
  every weapon, so the game barely uses deliberate missing. It is not inert,
  though: raised, it works (see the AT3 findings), which is why 1.1 applies it.
* **Difficulty touches no AI parameter.** The menu sends one integer
  (`new_game` 0/1/2, decoded from `difficulty_menu.swf`) and there is no
  difficulty field in the reflection schema at all.

## Reverse-engineering instruments

These exist because nearly every wrong turn in this project came from reasoning
about what must be true, and nearly every advance came from measuring it.

**Memory:** `peek`, `dumpaddr`, `probe`, `invdump`, `find`, `findval`,
`narrow`, `poke`, `freeze`, `unfreeze`, `unfreezeall`, `anchor`

**Prefs records (1.1):** `prefs find|get|set|dump|fields` - see
[the live prefs editor](#the-live-prefs-editor---prefs-prefstxt-knockback-11)

**Watchpoints (hardware DR0-DR7):** `watchaddr` (what writes here?),
`watchrw` (what reads this?), `watchexec <rva> [once]` (is this code reached?),
`watchoff`, `watchinv`

**Objects and classes:** `whatis` (RTTI), `instances`, `nearby`, `vtscan`,
`ctxinfo`, `vcall`, `diff`, `difftypes`

**Animation:** `granny` (find bone poses), `anim`

**Tracing:** `spy`, `grantspy`, `grantlast`, `npcspy`, `npchits`, `watch`,
`uispy`, `buildtest`, `spawngate`, `npccache [rebuild]` (SWSE's own live-NPC
list, which triggers count from when hit reactions are off - 1.1)

**Session:** `autoprime`, `remote`, `selftest`, `perf`, `agentdebug`, `exit`
(quits the game so the DLL can be replaced)

---

## Offline tooling (`oddforge/` here; `tools/` in the development repository)

`oddforge/`, `studio.py` and `swse_setup.py` are in this repository. The
`tools/` scripts below are not: they live in the development repository and
are listed so the pipeline is documented.

| Tool | What it does |
|---|---|
| `oddforge/` | SMB container + TOC parser, byte-identical round-trip on all 1,222 archives |
| `tools/texmap.py` | map every texture to its runtime fingerprint; `--fp-in` names what is on screen, `--match/--fp-out` builds fingerprint lists |
| `tools/census_textures.py` | how many textures exist, by format and size |
| `tools/hd_export.py`, `hd_pack.py` | the HD texture pipeline |
| `tools/downscale.py` | 4x→2x |
| `tools/seam_check.py`, `seam_diff.py`, `seam_fix.py` | detect and repair tiling seams broken by AI upscaling |
| `tools/alpha_check.py` | find DXT1 1-bit-alpha cutouts |
| `tools/reflect_dump.py` | the game's reflected fields, with exact offsets |
| `tools/reflect/` | exact reflection extraction from `stranger.exe` (`rtti_all.py`, then `extract2.py`) → `REFLECT_FIELDS.tsv` |
| `tools/gen_reflect_header.py` | `REFLECT_FIELDS.tsv` → `swse/reflect_gen.h` for the prefs editor |
| `tools/shot.ps1` | capture the game window, to verify visual changes directly |
| `tools/swse.ps1` | drive the in-game console from the command line |
| `tools/relaunch.ps1` | restart via the launcher, click PLAY, load the last save |
| `tools/laa_patcher.py` | large-address-aware patch |

**Starting the game.** `tools/relaunch.ps1` starts it through `Launcher.exe`
and presses PLAY. Starting `bin\stranger.exe` directly works in background mode
(`SWSE_AGENTDEBUG=1`) - every agent run of 1.1 did - and the "frozen window"
once blamed on it was most likely the cursor loop 1.1 fixed. A direct start
without background mode has not been re-measured. See
[TOOL_CONTRACT.md](TOOL_CONTRACT.md) for how a tool should start it.

---

## Files SWSE reads and writes

A mod is any folder under `SWSEMods\`; what it does is decided by the files it
contains. **Additive** files are read from every enabled mod in load order;
for a **last-wins** file the last enabled mod that has one decides.
`load_order.txt` sets the order and `!Name` disables a folder; a folder not
listed is enabled and loads after the listed ones, alphabetically.

```
<game>\bin\
  dinput8.dll                  SWSE itself
  dinput8_real.dll             optional - the real system DLL
  swse_log.txt                 runtime log; each session starts with
                               "==== SWSE injected ====", version and time
  swse_log.old.txt             the previous log, once it passed 4 MB (1.1)
  swse_selftest.txt            self-test results (appended)
  swse_shaders.txt             shaderdump output
  swse_frame_textures.txt      foliage scan output
  swse_wind_dump.txt           wind dump output
  swse_snap.tga                screenshots
  swse_meshes.odv, .tex        meshdump output (for Oddview)

<game>\SWSEMods\
  features.txt                 which systems are on (only the console, by default)
  load_order.txt               which mod folders are on, and their order
```

| File (in a mod folder) | Merge | Read by |
|---|---|---|
| `aiprefs.txt` | last wins | AI tuning (`aituning`, `difficulty`, `aitune`) |
| `playerprefs.txt` | last wins | player tuning (`playertune`) |
| `prefs.txt` | additive | prefs editor (`prefsedit`); `prefs keep` writes `SWSE Console\prefs.txt` |
| `characters.txt` | additive (1.1) | character tuning (`npctuning`, `tuning`) |
| `console.txt` | last wins | character-tuning rules (`npctuning`, `tuning`) |
| `triggers.txt` | additive | triggers |
| `positions.txt` | additive; `writepos` appends to `SWSE Console\positions.txt` | named positions |
| `sites.txt` | additive; `writepos` appends to `SWSE Console\sites.txt` | named positions, AT3 placements |
| `foliage.txt` | additive | foliage wind |
| `hitreact.txt` | last wins | hit reactions |
| `graphics.txt` | last wins; `set` writes to the winning file | graphics (legacy fallback: `SWSE Graphics\settings.txt`) |
| `pointers.txt` | last wins | `ptr` / `get` / `hold` |
| `textures\*.oft` | additive, later wins per fingerprint | HD textures |
| `scripts\*.txt` | last folder wins | script commands |
| `plugins\*.dll` | additive; for one name, the later mod's DLL is used (1.1) | native plugins, each loaded only when its switch is on |

Fixed paths, not looked up through the mod folders:

```
SWSEMods\SWSE Wind\wind.txt           wind settings (read when foliage starts)
SWSEMods\SWSE Console\binds.txt       key binds
SWSEMods\SWSE Console\aliases.txt     aliases
SWSEMods\SWSE Console\remote_in.txt   command mailbox
SWSEMods\SWSE Console\remote_out.txt  replies
```

---

## Research notes

The deeper write-ups are kept in the development repository's
`swse/research/`; this repository carries only `REFLECTION_SCHEMA.md` (which
the Mod Loader's Game Data tab reads). Ask in `#mod-dev` on Discord about
one. For native plugins (1.1):
`PLUGIN_SYSTEM.md` (the design, every rule's reason, the hook list) and
`PLUGIN_QA.md` (what the fault guard can and cannot catch, measured, and the
loader's test results). Earlier:
`AI_SYSTEMS.md`, `FOLIAGE_WIND.md`, `ANIMATION.md`, `NPC_TUNING.md`,
`NPC_SPAWNING.md`, `GRAPHICS_RTGI.md`, `HD_TEXTURES.md`, `ALL_FUNCTIONS.md`,
`SCRIPT_VM.md`, `CONSOLE_CALLABLE.md`, `BLOOD_DECALS.md`, and new in 1.1:

* `PLAYER_FACING.md` - where the player's facing lives, how SWSE reads and sets
  it, and the yaw convention.
* `FREECAM.md` - the developers' fly camera left in HD, how `freecam` installs
  it, and how Stranger's input is held while it flies.
* `PREFS_EDITOR.md` - the prefs object model, the resource registry, ammo and
  knockback semantics, and the 1.0.x NPC offsets the exact table corrected.
* `REFLECT_FIELDS.tsv` - every reflected field: class, name, offset, type.
* `AT3_DISCOVERIES.md` - everything Stranger: Armed to the Teeth found, mapped
  onto live offsets, with proposed commands (static analysis, not yet run in
  the game).
* `PLAYNPC.md` - playing as a character: the design, the engine's streaming
  rules it relies on, and the live tests.
* `GRAPHICS_ROADMAP.md`, `RT_1_3_PLAN.md` and the `RT_DIAG_*.md` notes - the
  ray tracer: the plan and the measurements.
* `RE_ENGINE_MAP.md`, `RE_RENDER_VISIBILITY.md` (camera and culling),
  `RE_SPAWNING.md` (how the game creates a character) and `RE_UI_AND_ICONS.md`
  (the Flash menus and their commands).
* `FACTIONS.md` - NPC-vs-NPC hostility and town raids: research, not a
  feature.
