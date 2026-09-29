# SWSE tool contract, version 1

What a tool can rely on when it drives Stranger's Wrath through SWSE: the
mailbox, the `query` answers, the feature switches, and the files SWSE reads
and writes. Stranger: Armed to the Teeth (AT3) is the first tool built on it.

**Stability promise.** Everything on this page holds for every 1.x release,
starting with 1.1:
- A change that breaks it bumps the contract version (`query contract`) and is
  listed at the top of the CHANGELOG.
- Additions (new `query` keys, new commands, new switches) do not bump it, so
  tools must ignore keys they do not know.
- Anything not on this page (console output wording, log lines, other
  commands' text) may change in any release. Parse `query`, not prose.

## 1. Finding SWSE

| Question | How |
|---|---|
| Is SWSE installed? | `bin\dinput8.dll` exists and its version resource says InternalName `SWSE` (ProductName `SWSE (Stranger's Wrath Script Extender)`). PowerShell: `(Get-Item bin\dinput8.dll).VersionInfo.ProductVersion` gives, for example, `1.1`. Works with the game closed. |
| Which game folder? | Steam: `<library>\steamapps\common\Stranger's Wrath`. GOG: the path in `HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<id>` whose `gameName` mentions Stranger. `tools\swse_paths.ps1` does both, plus the override `SWSE_GAME_DIR`. |
| Where is the exe? | `bin\stranger.exe` on Steam. A GOG build may differ, so look in both `bin\` and the root. |
| Is it running with SWSE? | `bin\swse_log.txt` has a line starting `==== SWSE injected ====` for this session, and the mailbox (below) answers. |
| Which version is running? | `query version` answers `swse=<ver> build=<name>`. `build` is `Steam_HD`, or `unknown` for a game build SWSE does not recognise. There SWSE runs in safe mode: nothing that patches, calls or reads the game runs. The mailbox works, and so do SWSE's own commands (`query`, `status`, `features`, `help`, `mods`, `ver`, `plugins`, `hooks`, `selftest`, binds, aliases, `exec`, `after`, `repeat`, `wait`, key presses) and the switches `graphics`, `hdtextures`, `foliage` and `raytrace`. The level watcher does not start: `query player` answers `levelup=0 epoch=0 level=- safemode=1` and nothing more, for good. A command that reaches the game is refused with one line and no effect; the switches `hitreact`, `npctuning`, `aituning`, `triggers`, `playertune` and `prefsedit` refuse to come on. `SWSE_UNKNOWN_BUILD_OK=1` in the game's environment turns safe mode off (then `build=unknown` runs everything). |
| Which contract? | `query contract` answers `contract=1 swse=<ver>`. |

## 2. The mailbox

Folder: `SWSEMods\SWSE Console\`. SWSE polls `remote_in.txt` about 8 times a
second while the game draws frames. Background mode (below) keeps it drawing.

**Request** (`remote_in.txt`), ASCII:

```
<sequence number>
<command line>
<command line>
...
```

- The first line is an integer sequence number. The request runs when that
  number **or the file's write time** differs from the last one SWSE ran. The
  clean way is a new number per request: the one on disk plus one.
- **End the file with a newline.** SWSE runs a request only once it is
  complete: the file ends with `\n`, or it has not changed for one poll.
- Each following line runs exactly as if typed at the console:
  - up to 256 lines, each up to 239 characters;
  - lines starting `#` are skipped;
  - `a; b; c` runs a sequence;
  - a line `wait <ms>` delays everything after it.
- At launch SWSE takes the file already on disk as seen, so a leftover
  request never replays.

**Reply** (`remote_out.txt`, CRLF):

```
<sequence number>
> <command line>
<its output>
> <next command line>
<its output>
<<END>>
```

- A reply is complete only when it contains `<<END>>`. Wait for that, and for
  the first line to equal your sequence number.
- Lines delayed by `wait`, or scheduled by `after`, run later and print to the
  console, not into this reply.
- A command that crashes writes
  `*** FAULTED - command crashed (game survived) ***` and the game carries on.

**Background mode.** Set `SWSE_AGENTDEBUG=1` in the game's environment at
launch: start `bin\stranger.exe` from a process that has it, and the game
inherits it.
- Starting `bin\stranger.exe` directly works in background mode. It was
  verified on every agent run of 1.1. The "frozen window" that
  `tools\relaunch.ps1` warns about was most likely the cursor loop that 1.1
  fixed. A direct start WITHOUT background mode has not been re-measured.
- Start it OUTSIDE the app the user is typing in. Windows lets a process
  started by the foreground app take the foreground itself. WMI's
  `Win32_Process.Create`, with `ShowWindow = 4` (SW_SHOWNOACTIVATE), does
  this; its `EnvironmentVariables` replace the whole block, so pass the
  current environment plus `SWSE_AGENTDEBUG=1`.

SWSE then:
- keeps the game simulating and reading the mailbox while it is behind other
  windows;
- never takes the mouse or keyboard. If the game comes to the front without a
  click on its window or its taskbar button, SWSE hands the foreground
  straight back to the window that had it. A click still brings the game
  forward, and so does switching to it from the shell (Alt+Tab, Task View);
- mutes the game's own audio session (`mute off` restores it).

Park the window behind others; do not minimize it, because a minimized game
stops pumping window messages. `menu continue` loads the last save without
any key input.

## 3. `query` - machine-readable answers

One line of space-separated `key=value` pairs. Values contain no spaces.
Unknown keys may appear: ignore them.

| Command | Keys |
|---|---|
| `query player` (the default) | `levelup` (0/1), `epoch` (level counter), `level` (leaf name or `-`), `x` `y` `z` (world, Z up), `yaw` (degrees, see section 5), `health` `healthmax`, `stamina` `staminamax`, `moolah`. In safe mode (section 1) `levelup=0 epoch=0 level=- safemode=1` only |
| `query position` | `levelup`, `epoch`, `level`, `x`, `y`, `z`, `yaw`; in safe mode as `query player` |
| `query features` | `<switch>=on\|off` for every switch, live state. Native plugins' switches (1.1) are extra keys: ignore the names you do not know |
| `query plugins` | `<plugin>=on\|off\|faulted\|refused` for every native plugin found (1.1); an empty line when there are none. `faulted`: it crashed this session and stays off until the game restarts. `refused`: SWSE would not load it (`plugins <name>` says why) |
| `query version` | `swse`, `build` |
| `query contract` | `contract`, `swse` |

Keys whose value cannot be read right now (no level loaded, for example) are
left out rather than faked.

## 4. Feature switches

`SWSEMods\features.txt` has one `name = value` line per switch, and `#`
comments.
- Values are `on` / `off`. `aituning` and `playertune` also take `auto`, their
  default: each runs only while its own file asks for something.
- A missing file or line means the default: `console` on, `aituning` and
  `playertune` auto, everything else off.
- SWSE rewrites only the value on a switch's line and keeps every comment.
  **Edit it with the game closed.** While the game runs, use
  `features <name> on|off|auto` through the mailbox, which switches now and
  saves the file.

Switch names (stable): `console`, `graphics`, `hdtextures`, `hitreact`,
`foliage`, `aituning`, `triggers`, `npctuning`, `playertune`, `prefsedit`,
`raytrace`.

A switch SWSE cannot run reads `off` in `query features` for the session,
whatever the file says, and the file keeps its line: `raytrace` without
`graphics` (and `features graphics off` takes it off too), and in safe mode
(section 1) `hitreact`, `npctuning`, `aituning`, `triggers`, `playertune`
and `prefsedit`.

Native plugins (1.1) add one switch each, named after the DLL
(`SWSEMods\<Mod>\plugins\<name>.dll` is `<name>`), off unless the file says
`on`. A tool must not switch on a plugin it did not install: `features all on`
deliberately leaves plugins off.

## 5. Files SWSE reads that tools may write

| File | Where | Contract |
|---|---|---|
| `aiprefs.txt` | the last enabled mod folder that has one (normally `SWSE Console`) | `[profile]` sections of `key = value` multipliers (`misstime` alone is in milliseconds, 10 = vanilla). `active = <profile>` outside any section, or `off`. A decimal comma is read as a point. Up to 32 profiles in 64 KB. |
| `playerprefs.txt` | the last enabled mod folder that has one | `key = value` for `health`, `stamina`, `speed`, `jump`, `gravity`, `aircontrol`. Blank, `-` or `default` keeps the game's own value. |
| `positions.txt` | every enabled mod | `label x y z [level] [yaw]`, either order after `z`: a number is the yaw, a word is the level. A later label replaces an earlier one. |
| `sites.txt` | every enabled mod, read after `positions.txt` | `label x y z yaw [level]`: AT3's order. |
| `prefs.txt` | every enabled mod | `<target> <field> <value>`: live edits to prefs records, re-applied every level (prefsedit switch). |
| `triggers.txt` | every enabled mod | mod events. Syntax in SWSE_FEATURES.md. |

**Yaw convention** (positions, sites, `query`, `yaw`, `tpxyz`): the angle
level records store for placed objects, `atan2(m3, m0)` of the object's world
matrix, in degrees. 0 faces -Y, 90 faces +X; the compass heading is
yaw - 90. Z is up.

**Files SWSE writes** (tools may read them, never write them, except
`features.txt` with the game closed - section 4):
- `SWSE Console\positions.txt` and `sites.txt` (appended by `writepos`);
- `features.txt` (rewritten by `features`);
- `SWSE Console\binds.txt` and `aliases.txt`;
- `prefs.txt` in `SWSE Console` (by `prefs keep`);
- `bin\swse_log.txt` (rotated to `swse_log.old.txt` past 4 MB).
