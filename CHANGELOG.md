# Changelog

What changed in SWSE, release by release, newest first. The full command and
file reference is [SWSE_FEATURES.md](SWSE_FEATURES.md).

## 1.1

SWSE 1.1 switches on only the console by default, can switch every other
system on and off while the game runs, and adds player tuning, a live prefs
editor, facings saved with every position, and console automation. It also
adds:
- native plugins: code a mod can ship, run only when the user names it;
- playing as any character in the level (`playnpc`);
- a free-flying view, using the game's own developer camera (`freecam`);
- ray-traced ambient occlusion, as an experiment behind its own switch;
- menus a tool can drive without key presses;
- a written contract for tools built on SWSE;
- a background mode for running the game unattended.

Much of it was built to support **Stranger: Armed to the Teeth** (AT3) by
Racewizard, which is built on SWSE - see [AT3_INTEGRATION.md](AT3_INTEGRATION.md).

### Added - native plugins

- **A plugin is a DLL in a mod folder.** `SWSEMods\<Mod>\plugins\<name>.dll`,
  a 32-bit DLL exporting `SWSEPlugin_Query` and `SWSEPlugin_Load`, talking to
  SWSE only through the function tables of `sdk\swse_plugin_api.h` (plugin API
  version 1). It needs nothing of SWSE's source.
  - Found through the mod registry: `load_order.txt` orders and disables a
    plugin with its mod. When two enabled mods ship one name, the later mod's
    is used and `plugins` lists the other as shadowed.
  - `mods reload` and `plugins rescan` find plugins added while the game runs.
- **Each plugin is a switch, off until named.** `hello.dll` is the switch
  `hello`: `features hello on` (saved to `features.txt`), `hello = on` in the
  file, or `features hello on temp`.
  - While it is off, SWSE does not even load the DLL.
  - `features all on` never switches a plugin on; `features all off` switches
    plugins off too.
  - Off stops its callbacks; the DLL stays loaded until the game exits.
  - A plugin cannot take a built-in switch's name or nickname (nor `all` or
    `preset`), and is never `auto`.
- **What a plugin gets.**
  - Log lines, tagged with its name.
  - Console commands that work everywhere built-ins do (binds, aliases,
    triggers, the mailbox), and `Execute` of any console line. `Post` queues a
    line from a worker thread for the next frame.
  - Per-frame TICK and OVERLAY callbacks. OVERLAY runs before the console
    inside a GL state snapshot, so a plugin that leaves state bound cannot
    break the console.
  - Events: level up and down, `mods reload`, any switch changing, and each
    texture upload with SWSE's fingerprint.
  - A glBindTexture listener, from SWSE's own hook.
  - Its switch handlers, mod files, the level, the player, named positions,
    RTTI, and fault-safe memory reads and writes.
  - Its own lines in `selftest`, `perf` and `status`.
- **A fault costs the plugin, not the game.** Every call into a plugin is
  guarded and timed.
  - A fault switches that plugin off for the session and names the DLL and the
    offset: of the faulting instruction, of the code that threw, or of the
    plugin's own call when the fault was inside something it called.
  - `features.txt` is left alone, so a fixed build loads at the next launch;
    until then the plugin stays off.
  - A plugin whose Load fails has everything it registered rolled back.
  - Not caught: faults on a plugin's own threads, and heap corruption that
    crashes the game later.
- **`plugins [name|rescan]`** lists every plugin with its state, version, mod,
  frame cost, commands and SHA-256. **`query plugins`** answers
  `<name>=on|off|faulted|refused` for tools.
- **Provenance.** Before any of a plugin's code runs, `swse_log.txt` gets its
  path and SHA-256. An unloaded plugin's description and version are read from
  its version resource, without running it.
- **Stalls name the plugin.** A `FRAMESTALL` log line says which plugin owned
  a slow frame (`-> SWSE, plugin crashy6 121 ms`); `perf` shows every
  plugin's last, average and worst frame cost.
- **`hooks`** lists every code or table patch SWSE has live and the system
  that owns it: the frame hook, glspy's GL hooks, the glBindTexture hook,
  materials' glDrawElements, the hit-reaction and script-VM trampolines,
  input's vtable and import slots, uispy and playnpc. A plugin's `Memory.Write`
  refuses those ranges.
- **The SDK**, in `sdk\`: the header, a complete example
  (`examples\hello_plugin`), a stand-in host for trying a plugin outside the
  game (`tools\mockhost`), and the test plugins (`tools\qaplugins`) behind
  `tools\swse_plugin_tests.ps1`, the in-game test pass.
  `swse\build.bat` compiles the example too, as an ABI check.

### Changed

- **Console-only by default.** Only the console is on unless
  `SWSEMods\features.txt` says otherwise; a missing file or a missing line now
  means **off** (1.0.x: on). Every system is still built in - nothing was
  removed - and a system that is off installs no hooks at all, so a fresh
  install leaves the game exactly as shipped apart from the console.
- **The download carries `SWSE Setup.exe`, not the Mod Loader.** Players
  install, update and switch SWSE with it (see Added). The Mod Loader -
  textures, values, load order - is the modders' tool now: `studio.py` in the
  repository, built with `ModLoader.spec` (1.0.x kept a prebuilt
  `ModLoader.exe` in `release\`).
- **`features` switches systems live**: `features <name> on|off` switches now
  and saves to `features.txt`; add `temp` for this session only;
  `features all on|off` covers everything but the console; nicknames such as
  `wind`, `hd`, `gfx`, `ai` are accepted. `features` on its own lists each
  switch with what it controls. 1.0.x could only read the file at launch.
- **Typing in the console no longer drives the game.** While the console is
  open, the game gets no key presses, mouse movement, clicks or wheel:
  typing never walks Stranger and the mouse does not turn the camera. That
  includes keys the game reads just after the console closes but that were
  typed while it was open, such as the key that closed it. Key releases
  still pass, so nothing is left held down, and key binds (console closed)
  are unaffected. 1.0.x passed everything through. `inputst` has a second
  line, `guards:`, that counts what was kept back.
- **The console reads the keyboard only while the game is the window you are
  in.** It read the global key state, so with the console open (after a
  mailbox `show`) typing in any other app went into its line and Enter ran
  it, and `~` toggled it from other apps too.
- **Eleven switches** in `features.txt`: `console`, `graphics`, `hdtextures`,
  `hitreact`, `foliage`, `aituning` and `triggers` as before, plus the new
  `npctuning`, `playertune`, `prefsedit` and `raytrace` (below). The shipped
  file lists them all, with a paragraph each.
- **Upgrading:** a 1.0.x `features.txt` still works - its `on` lines mean on -
  but `triggers`, which 1.0.x switched on because the file did not mention it,
  is now off unless a line says `triggers = on`.
- **`misstime` in `aiprefs.txt` is applied** (it was parsed and ignored). It is
  given in the game's standard milliseconds rather than as a multiplier -
  `misstime = 10` is vanilla, as Stranger: Armed to the Teeth writes it - and
  each firing weapon scales from its own shipped value (shipped × misstime /
  10), so a weapon that ships 0 stays 0. Leaving it out keeps the shipped
  values.
- **`aituning` and `playertune` ship as `auto`.** Each runs only while its own
  file asks for something - an `active = <profile>` line in `aiprefs.txt`, or
  any value in `playerprefs.txt` - and stays off otherwise. A stock install
  asks for nothing, so it stays console-only; AT3's Global Rules (which write
  those two files) work without editing `features.txt`. An explicit `on` or
  `off` always wins; `features <name> auto` sets it back and re-checks now.
  The `active` profile applies 4 s after each level's player appears.
- **The shipped character-tuning rules are commented out.** `console.txt`'s
  `noimmortals` / `immortalsgib` and the 22 rules in `characters.txt` are now
  examples: live, they made every townsperson, native and storekeeper mortal
  for anyone who switched `npctuning` on for their own rules.
- **Facing is saved with positions.** `savepos` / `tp` keep the facing as well
  as the spot; `savepos <label>` and `tp <label>` are `writepos` and `goto`;
  `pos` prints the yaw; `writepos`, `goto` and `positions` save, restore and
  show it. `positions.txt` gains an optional yaw column (after the level, so
  1.0.x still reads the files), and `writepos` also writes `sites.txt` in the
  column order AT3 reads (`label x y z yaw [level]`) when the facing is known.
  The yaw is the rotation angle level records store for placed objects
  (`atan2(m3, m0)`: 0 = facing -Y, 90 = +X), so it means the same in SWSE, AT3
  and the game's own files.
- **`writepos` always writes `SWSE Console\positions.txt`.** 1.0.x appended to
  whichever mod last had a `positions.txt` - in a stock install the shipped
  `SWSE Ambushes` example, which an update replaces, and not where AT3 looks.
  Every mod's `positions.txt` is still read.
- **Named positions load at launch** whatever the switches say (1.0.x loaded
  them only with triggers on), and `sites.txt` is read alongside
  `positions.txt` from every enabled mod.
- **The remote mailbox** runs a request when its sequence number **or the
  file's write time** changes, so a tool that restarts its numbering each
  session (AT3 does) is no longer ignored; and it runs a request's lines as one
  sequence, so `wait <ms>` on a line of its own delays the lines after it.
- **The log keeps itself tidy.** At launch, a `bin\swse_log.txt` over 4 MB
  becomes `swse_log.old.txt`, replacing the previous one, and a fresh log
  starts. 1.0.x appended forever.
- **Each session in the log is identified.** Its first line is
  `==== SWSE injected ==== version 1.1, <date> <time>, dll link stamp <hex>`,
  and the game build follows at the first frame. A pasted log now says what
  ran and when.
- **Character tuning is the `npctuning` feature.** It reads `console.txt` and
  **every** enabled mod's `characters.txt` (additive, as the mod registry always
  documented; 1.0.x read only SWSE Console's). `tuning` does the same by hand,
  for the session.
- **`gfx`** refuses while the graphics feature is off and says how to turn it
  on.
- **Script commands** (`scripts\*.txt`) honour `wait <ms>` lines.
- A console command may now have up to 32 words (was 16), so alias, bind and
  `after` bodies fit.
- `help` has three new categories: `tuning` (`features`, `playertune`,
  `prefs`, `knockback`, `npc`, `wpn`), `raytrace` and `input`. The ray-tracing
  and input commands used to show only on an "other" line, and
  `help input` found nothing.
- `ver`, the welcome line and `status` say `1.1`.
- `hd` reports the installed count as soon as HD textures are switched on (the
  folder is indexed then, not at the first texture upload).
- `SWSE-install.bat` is described as optional in the install guide: since 1.0.1
  SWSE finds the system `dinput8.dll` itself.
- **The glBindTexture hook belongs to the core** (glspy.cpp), not to foliage.
  Foliage is its first client and behaves as before: same hook, same check of
  the function's first bytes, same log line. Plugins' bind listeners ride the
  same hook.
- **`features` lists plugin switches** after the built-ins; `query features`
  and `status` include them. `help` lists plugins' commands under their plugin,
  `help <plugin>` shows them, and an alias cannot take a plugin command's name.

### Added

- **SWSE Setup: SWSE installed and switched with tick boxes.** `SWSE Setup.exe`
  in the release zip (`swse_setup.py`) is one window; the modders' Mod Loader
  (`studio.py`, no longer in the player zip) shows the same screen as its
  first tab (`oddforge/setupui.py`). It:
  - finds the game as `tools/swse_paths.ps1` does - `SWSE_GAME_DIR`, every
    Steam library, the GOG registry and folders - with Browse for another,
    and shows which SWSE is installed there (`bin\dinput8.dll`'s version);
  - one tick box per switch with its one-line description (auto, on or off
    for aituning and playertune), the plugins found in the enabled mods, and
    three presets: Recommended (the 1.1 default), Classic SWSE (`features
    preset full`) and Everything;
  - **Install / Update** copies `bin\dinput8.dll` and `SWSEMods` from the
    unzipped release it runs from, then writes `features.txt` from the ticks.
    It never overwrites the user's own files - binds, aliases, sites, prefs,
    playerprefs and positions in SWSE Console, `features.txt`,
    `load_order.txt`, a mod folder they added - or an `aiprefs.txt` that is
    not the shipped one (AT3 writes it). A shipped file they edited, and a
    `dinput8.dll` that is not SWSE's, are moved to `SWSE-backup-<date>` in the
    game folder first. It refuses while the game runs (the DLL is locked);
  - **Apply switches** rewrites only `features.txt`, and with the game running
    also switches them live through the console mailbox (`features <name>
    on`);
  - **Uninstall** moves `bin\dinput8.dll` and `SWSEMods` into
    `SWSE-uninstalled-<date>` in the game folder. Nothing is deleted;
  - ticking HD textures shows whether the pack is in `SWSE HD\textures` and
    offers the 4GB patcher when the exe still needs it.

  The logic is in `oddforge/`: `swsefeatures.py` reads and writes
  `features.txt` by `swse/features.cpp`'s rules (checked byte for byte against
  `features.cpp` itself on 16 inputs), `swseinstall.py` and `gamepaths.py`,
  with `tools/test_swsefeatures.py`. `"SWSE Setup.exe" --selftest <file>`
  checks a build starts, without showing a window; `tools/package_swse.ps1`
  runs it before zipping, and refuses an exe older than its Python.
- **`features preset full|default|list [temp]`**: several switches in one
  word.
  - `full` is the classic SWSE, the 1.0.x look: console, graphics,
    hdtextures, hitreact, foliage and aituning on; every other switch as it
    is.
  - `default` is the 1.1 default: console on, aituning and playertune auto,
    everything else off, plugins included.
  - Saved like `features`, or for the session with `temp`. In safe mode the
    switches it refuses are left as they are and named in one line. It says
    which switches take full effect only from the next level load
    (hdtextures, hitreact, foliage).
  - `all` and `preset` are the command's own words now: no plugin may take
    either name.
- **`features.txt` reads as a switchboard.** Three lines of header, then the
  eleven switch lines, each with a short note (`foliage    = off    # grass
  and plants sway in the wind`); the long explanations moved below them, as a
  reference. Saving keeps a switch line's trailing note, as it always did.
- **Level watcher** (`levelwatch.cpp`): "a level is up" as a signal of its own,
  taken from the player object and the motion objects it owns. Player tuning
  (3 s), AI tuning and triggers (4 s), prefs edits (4.5 s) and the self-test
  (5 s) run once per level, that long after the player appears.
- **Player tuning** - `playerprefs.txt` and `playertune [reload|apply|restore]`:
  `health`, `stamina` (max and current), `speed`, `jump` (the player's own
  motion objects), `gravity`, `aircontrol` (global motion prefs). Blank keeps
  the game's value; re-applied if the game resets max health or stamina;
  `playertune restore` or switching the feature off puts the game's own values
  back. The release ships only a blank template
  (`SWSE Console\examples\playerprefs.txt`), which changes nothing.
- **Live prefs editor** - `prefs find|get|set|keep|dump|fields|restore|reload|status`
  and `prefs.txt`: read and write any loaded prefs record by path, path hash,
  RTTI class or address, and any field by reflected name, `Class::name`, dotted
  embedded field, vector component or offset. Records are found through the
  game's own resource registry - a sorted list of every loaded resource,
  binary-searched, read-only. Every original value is captured, so
  `prefs restore` and switching the feature off put the shipped numbers back.
  Nothing on disk changes.
- **An exact reflection table** - `tools/reflect/` extracts every reflected
  field from `stranger.exe` by emulating the game's own reflection
  initialisers: `swse/research/REFLECT_FIELDS.tsv`, 5,697 fields in 191
  classes with exact classes, types and inheritance. `tools/gen_reflect_header.py`
  turns it into `swse/reflect_gen.h` for the prefs editor. It replaces
  `FIELD_OFFSETS.tsv`, whose offsets were right but whose class was right on
  only 689 of 1,707 rows.
- **`npc` and `wpn`** - any character or weapon field by a friendly name, on
  top of the prefs editor (restorable, `keep` for every level): `npc <type>`
  shows a character's main fields and `npc <type> <field> [value]` sets one -
  species, gib effect, gib-spawn, loot drops (`loot.death`...), mass, run
  speed, jump, the four sight cones (`sight.combat.see`...), the 42 combat
  tactics (`attack.<param>`) and more; `wpn <ammo|npcweapon|character hash>`
  does the same for damage, clip, area of effect, range, speed, gravity,
  homing, bounce, knockback, fire rate and reload, and `wpn all` for every
  player ammo type. Built from the AT3 discovery catalogue (P1-P10).
- **`playnpc` - play as another character in the level.**
  - `playnpc` lists the level's character types, and whether each is in memory
    now (`ready`), can be streamed in first (`load`), only comes with its own
    area (`far`), or has no body in this level (`nobody`).
  - `playnpc <name>` makes the player that character: its body and its own
    animation set. The gaits it lacks are filled from its own clips: walk,
    trot, canter, run, turnarounds, skids, bursts, landings and jumps. So it
    moves like itself whatever Stranger is doing, and never plays a Stranger
    clip.
  - It moves at its own speed: walk, trot and run come from its own clips.
    At full speed in Act 1, where Stranger can't yet run on all fours, a
    character whose trot, canter and run share one clip used to snap to its
    bind pose (a T-pose). The engine's disguised-Stranger path divided zero
    by zero on those gaits. A guard now catches the bad weights for the
    played character alone and keeps its fastest clip playing. Stranger, the
    NPCs and the later acts are untouched.
  - It has its own health and stamina. Townsfolk, natives and the other
    characters the game never lets die are capped at Stranger's. Switching
    characters, or going back, keeps the fraction you had, so a 30%-health
    outlaw comes back as a 30%-health Stranger. Nothing of it is saved.
  - It wears what the game gives it: hats, quivers, and its weapon (the outlaw
    mortar's mortar). Before this, nothing it wore was put on.
  - Everything it wears, and its own body, shows its real textures. Some
    pieces borrow a texture from another character's data - boilzbooty's and
    Filthy Hands Floyd's shotgun, the outlaw shooter's hat - and those drew
    pure white or flat grey. SWSE now loads and keeps those too, the way the
    game does for its own characters, before the character goes on.
  - Stranger's attacks are all off while you play a character, whatever the
    camera does: the crossbow (firing, reloading, its punch and switching
    ammo), the buck and the headbutt (out of stamina too), and ramming into
    things. The sprint stays. The character's own melee and weapon are
    coming.
  - Dying plays the character's own death animation, then the usual
    checkpoint reload brings Stranger back.
  - `playnpc anims`, `playnpc runtest [ms] [sprint] [buck]`, `playnpc hurt
    <n> [stamina]` and `playnpc textures [name]` are QA tools. The first two
    sample the animation blend in swse_log.txt; `runtest` drives a run with W,
    with `sprint` also the right mouse button, and with `buck` a left click.
    `hurt` takes n off the current health or stamina. `textures` lists each
    material of the body and of what it wears: where its texture lives, and
    whether that is loaded.
  - Stranger spins into the character with his own transformation; `nospin`
    swaps instantly. `playnpc off` spins back; `off nospin` is instant.
  - A character that is not in memory is streamed in first ("loading ...").
    Measured: 31-125 ms for each of six loads across four levels.
  - While it is played, its data stays in memory the way the game keeps a
    live character's. Walking away from its area no longer turns the player
    back.
  - While playing a character: no double jump; no first-person or sniper view;
    no Stranger attacks (see above); no manual Stranger/Steef switch; no foot
    IK.
  - `bare` leaves its hat or weapon off. `scale <x>` draws it at another size,
    from 0.3 to 1.9. The Giant Sleg is drawn at 1.9x, because the PC port's
    bone packing wraps at 2x.
  - `playnpc status` and `playnpc states` show what is installed and held.
  - A level change, death or checkpoint returns you to normal. Saves are
    unaffected: the body is not saved.
  - Off until used. Refused on a game build SWSE's addresses were not
    measured on. The design and the live tests are in
    `swse/research/PLAYNPC.md`.
- **`freecam` - fly the view with the game's own developer camera.**
  - `freecam on` detaches the view from Stranger. The camera is the game's
    FlyCamera, left in the HD build with its controls intact: the move keys
    or stick fly, and a gamepad's right stick turns. The key that switched it
    on in the developers' build is gone; SWSE installs it the way that key
    did.
  - The mouse turns the view, and so do the arrow keys. The developers'
    camera is older than the PC version: it reads one frame's mouse motion as
    a slow turning speed, so on its own the mouse barely turned it. SWSE turns
    it by the mouse's raw motion instead.
    - `freecam sens [n] [updown]` sets how far, in degrees per 100 mouse
      counts, times the game's own mouse sensitivity (default 8).
    - A negative up/down value inverts the vertical.
  - Stranger stays where he is and takes no input while the view flies. If a
    movement key is held when freecam starts, he keeps his controls until it
    is released, so he is not left running.
  - `freecam off` returns to the player camera exactly as it was.
    `freecam toggle` is for `bind`.
  - With no real input, for tools and background mode:
    - `freecam pos <x> <y> <z>` places the camera;
    - `freecam look <yaw> [pitch]` aims it, in degrees, with yaw as `yaw`
      reports it and pitch positive up;
    - `freecam speed [n]` sets the flying speed in units per second
      (default 40);
    - `freecam` on its own prints the position, angles and speed.
  - It switches itself off on a level change or load, a death and a
    cutscene.
  - Off until used. Refused on a game build SWSE's addresses were not
    measured on. The design is in `swse/research/FREECAM.md`.
- **`raytrace`, an eleventh switch (experimental).** The ray-traced ambient
  occlusion from the graphics line, with its commands (`rt`, `harvest`, `geo`,
  `gpu`, `progsrc`, `ssrmask`) and the debug views `skin` and `nohat`.
  - Off by default, and it runs inside the graphics pipeline:
    `features raytrace on` refuses while `graphics` is off. With `graphics`
    off, none of it loads. `raytrace = on` in `features.txt` without
    `graphics` is refused the same way at launch: the switch reads off, and
    `swse_log.txt` says why.
  - It is the master switch. `rtao_enable` in `graphics.txt` does not start
    the tracer on its own.
  - With it off, `rt`, `harvest` and `geo` say so instead of running.
  - Its occlusion is known to be wrong: it is built from what the camera
    already drew, so anything off-screen casts nothing. The next milestone
    fixes that (`swse/research/RT_1_3_PLAN.md`). See "Ray tracing
    (experimental)" in SWSE_FEATURES.md.
- The prefs editor no longer falls back to another class's offset for a field
  the object's own (fully described) class does not have - it refuses instead.
- **`knockback`** - lists all 31 player ammo types with their live
  `m_maxKnockSpeed` / `m_maxKnockSpeedPlayer`, and sets them
  (`knockback boombat 40 25`, `knockback all - 0`, `keep` to re-apply every
  level): the live equivalent of AT3's bundle patches.
- **`yaw [deg]`** and **`tpxyz <x> <y> <z> [yaw]`**.
- **Automation:** `;` runs several commands from one line; `wait <ms>` inside a
  sequence, alias or script; `after <sec> <command>`; `repeat <n> <command>`;
  `exec <file>`; `log <text>`.
- **Key binds** - `bind <key> <command>`, `unbind`, `binds`, saved in
  `SWSE Console\binds.txt`.
- **Aliases** - `alias <name> <commands>` with `%1`..`%9` / `%*`, `unalias`,
  `aliases`, saved in `SWSE Console\aliases.txt`.
- **For tools:** `status`; `query player|position|features|version|contract`,
  one line of key=value; `mods [reload]`; `hide` / `show` for the overlay.
- **The tool contract.** [TOOL_CONTRACT.md](TOOL_CONTRACT.md) lists what a
  tool can rely on for every 1.x release: finding SWSE and the game folder,
  the mailbox protocol, the `query` keys, the switches, and the files a tool
  may write. `query contract` answers `contract=1 swse=1.1`.
  - A breaking change bumps the contract number and is listed at the top of
    this file.
  - Additions do not bump it.
- **Menus without keys.** `menu list`, `menu continue`, `menu skip`,
  `menu resume` and `menu fs <screen> <cmd> [arg]` send a Flash screen its own
  command, the way the screen's button does. Nothing is typed, so they work
  with the game behind other windows.
  - `continue` uses `menu continue` when the main menu is up, and key presses
    otherwise.
  - `menu <n>` keeps its key-press form.
- **Diagnostics:** `npccache [rebuild]` shows SWSE's own live-NPC list (what
  triggers count from when hit reactions are off); `prefs registry [path|hash]`
  traces a lookup in the game's resource registry.
- **Character-mesh debug commands:** `meshdump [stop]` captures character
  meshes for Oddview (`bin\swse_meshes.odv`); `hidepart`, `noponcho` and
  `nodreads` hide one part of a character's model, as `nohat` does. They ride
  the graphics pipeline's draw hook, so they need `graphics` on, and say so
  when it is off (with `skin`), rather than arming something nothing runs.
- **A game-build check, and a safe mode** (`gamebuild.cpp`). Every address
  SWSE uses inside `stranger.exe` was measured on one build: the Steam
  release of Stranger's Wrath HD. SWSE now identifies the running exe from
  its PE header (link time, image size, entry point) before trusting those
  addresses.
  - On any other build (the GOG release, a future patch), SWSE runs in safe
    mode: nothing that patches game code, calls a game function or reads the
    game through one of those addresses runs. Writing into the game's code
    there is the likely cause of the reports that the console crashes the GOG
    version. A GOG exe has not been tested yet.
  - What works in safe mode:
    - the console and the mailbox, and the commands that use only SWSE
      itself: `help`, `status`, `query`, `ver`, `features`, `mods`,
      `plugins`, `hooks`, `selftest`, binds, aliases, `exec`, `after`,
      `repeat`, `wait`, `snap`, `mute`, background mode, and the key presses
      (`key`, `newgame`, `continue`, `skipcut`, `menu <n>`);
    - the OpenGL-side switches and their commands: `graphics`,
      `hdtextures`, `foliage` and `raytrace`. The plants sway but do not part
      around the player: that needs the player's position;
    - the memory readers that read only an address you type (`peek`,
      `dumpaddr`, `whatis`, `diff`, `instances`, `vtscan`, `findval`,
      `narrow`);
    - native plugins, whose API calls that would reach the game answer
      `SWSE_E_UNSUPPORTED` (the player's position, facing, health and
      stamina, `Memory.Write`) or 0 (the path hash, the resource lookup).
  - What does not:
    - the level watcher does not start, so `query` answers `levelup=0` and
      `safemode=1` and no player keys;
    - the switches that act on the game refuse to come on, at launch or
      live: `hitreact`, `npctuning`, `aituning`, `triggers`, `playertune`
      and `prefsedit` (`features` marks them `refused`);
    - the console refuses every command that reaches the game - 150 of the
      220 built-ins: the player, movement, items, world, tuning (but
      `features`) and music commands, most debug ones, `call` and the game's
      functions by name, and the `pointers.txt` chains (`ptr`, `get`,
      `hold`, `set` on a chain) - each with one line saying so, and `help`
      marks them with a `*`. `menu` refuses its keyless half; `continue`
      falls back to key presses.
  - The log names the build at the first frame. `status` shows it and what
    safe mode runs, `query version` answers `swse=1.1 build=Steam_HD` (or
    `build=unknown`), `hooks` lists the patches refused, and the self-test
    heads its report `unknown build: safe mode`, checks what runs, and lists
    the rest on a `[SAFE]` line.
  - `SWSE_UNKNOWN_BUILD_OK=1` in the game's environment turns safe mode off,
    for testing a build once its addresses have been checked.
- **Background mode for tools.** Start the game with `SWSE_AGENTDEBUG=1` in
  its environment, and SWSE switches AgentDebugMode on at the first frame and
  mutes the game. A tool can then run the game unattended behind other windows
  and drive it through the mailbox; `continue` loads the last save that way.
  - Key presses a tool injects are held for whole game frames, so they
    register at any frame rate.
  - The game no longer takes the keyboard. It used to come to the front
    after a level load while the user typed in another app, and take their
    keys. Now, if it comes to the front without a click on its window or its
    taskbar button, SWSE hands the foreground straight back to the window the
    user was in. The log line starts `background: the game came to the front
    without a click`.
  - A click on the game window or its taskbar button still hands it over, and
    so does switching to it from the shell: Alt+Tab, Task View or the Start
    menu. Only a switch none of those made - the game taking the foreground by
    itself - is handed back.
  - Park the window behind the others. Do not minimize it: a minimized game
    stops handling window messages.
- **`mute [on|off]`**: the game's own per-app mute (its entry in the Windows
  volume mixer), live.
- **Tools:**
  - `tools/swse_paths.ps1` finds the game in every Steam library, the GOG
    registry and GOG Galaxy folders, or at `SWSE_GAME_DIR`. `swse.ps1`,
    `swsecmd.ps1`, `relaunch.ps1` and the QA suite use it.
  - `tools/threadsample.ps1` samples a game thread's instruction pointer as
    module+offset. It found the freeze below.
- 41 new console commands in all: 220, up from 179.
- **A version resource in `dinput8.dll`** (file version 1.1.0.0, product
  version `1.1`), so tools can read the installed version without running the
  game - `(Get-Item bin\dinput8.dll).VersionInfo.ProductVersion`, or the file's
  Properties > Details.
- Release files: `SWSE Console\examples\` holds commented templates for
  `playerprefs.txt`, `prefs.txt`, `binds.txt` and `aliases.txt`. They are
  examples rather than live files so that unzipping an update never
  overwrites yours (or the `playerprefs.txt` Stranger: Armed to the Teeth
  manages); the commands create the real files when first used.
- `aiprefs.txt` is read up to 64 KB (1.0.x: 8 KB, which a couple of extra
  profiles after AT3's appended `[Racketeer]` section would have cut off).
- Key binds on middle mouse / mouse 4 / mouse 5 (`mouse3`..`mouse5`).
- Docs: this changelog, [AT3_INTEGRATION.md](AT3_INTEGRATION.md),
  `swse/research/PLAYER_FACING.md`, `swse/research/PREFS_EDITOR.md` and
  `swse/research/AT3_DISCOVERIES.md`. `swse/research/FACTIONS.md` and
  `OVERNIGHT_LOG.md` are restored from the July snapshot, with a header
  reconciling them with what AT3 found since. `FEATURES.md` and `ROADMAP.md`,
  both from the 1.0 era, are now short pointers to the current docs.
- Engine research:
  - `swse/research/RE_ENGINE_MAP.md`;
  - `RE_RENDER_VISIBILITY.md` (camera and culling);
  - `RE_SPAWNING.md` (how the game creates a character);
  - `RE_UI_AND_ICONS.md` (the Flash menus and their commands);
  - `tools/re`, which names 3,382 functions of the HD exe from the beta's
    debug symbols.

### Fixed - latent 1.0.x bugs

Several of these were hidden while every system defaulted on and showed the
moment one could be off; the rest came out of the exact reflection table.

- **Teleport commands now move the player.** `tp`, `goto`, `tpxyz`, `up`
  and `move` never moved the player in any release (owner, 2026-09-28).
  - Why: they wrote a copy of the position that the game rewrites from
    physics every frame, so "moved to" was only the console's word.
  - They now use the game's own teleport, the call its checkpoint respawn
    makes. It moves the physics, streams the destination in first (a short
    hitch on a long jump), and updates the player's zone.
  - `tp` returns to the zone that `savepos` recorded. Any other destination
    uses the zone whose box contains the point; a point outside every zone
    is refused rather than guessed.
  - `tp`, `goto` and `tpxyz` stand you on the floor at the spot.
  - `up` lifts you exactly and lets you fall. Its default is now 10 units
    (it was 100 when nothing moved), and there is no ceiling check, so use
    it in the open.
  - A teleport is refused, with the reason, on an unknown game build, while a
    level loads, and in a boat (the game ignores teleports there).
  - Every teleport logs the zone it chose, and how, in `swse_log.txt`.
  - `pos` and `query` now read where the feet are the engine's way. Right
    after `playnpc` swaps a body they no longer read 0,0,0.2, and with no
    body they leave the position out.
  - See `swse/research/TELEPORT.md`.
- **Foliage wind never identified a plant unless graphics or HD textures was
  on.** Plants are recognised by fingerprinting textures as they upload, in the
  upload hook the graphics and HD features installed; with both off, foliage
  had nothing to recognise. The foliage feature now installs the hook itself.
- **AI tuning's automatic apply, triggers and the automatic self-test depended
  on hit reactions.** All three waited for the hit-reaction actor list to fill
  as their "level is up" signal, and that list only fills with hit reactions
  on - so without them `active = <profile>` never applied and no trigger ever
  ran. They use the level watcher now, and triggers that count NPCs (`killed`,
  `cleared`) keep their own NPC list when hit reactions are off. That count
  also read only the first 64 characters, under-counting big levels. Verified
  live: `levelload`, `every` and `cleared` triggers fire with hit reactions
  off.
- **Every NPC health change also set that character's stamina.** `npchealth`,
  `allnpcs` and `characters.txt` wrote the value to the field after `m_health`
  as well, believing it was a maximum; the exact reflection table shows it is
  `m_stamina`. Only `m_health` is written now.
- **`ai <type> <field> <value>` wrote to the wrong places.** It read the
  character's `+0x118` as a hash when it is an embedded AIPrefs object, so it
  wrote into that object's vtable (forcing the page writable), wrote weapon
  fields through the weapon's hash as if it were an address, and its AI offsets
  were four bytes short. It now writes the embedded object and the weapon the
  character really uses, and `ai <type>` shows the decoded values. It also
  showed `firerate` as a time in milliseconds; it is shots per second.
- **Character-type lookups created records.** 1.0.x resolved a type hash
  through the game's `GetPrefs<NPCPrefs>` (RVA `0x23880`), which on a miss -
  any weapon or AI hash - constructs a default NPCPrefs carrying that hash,
  registers it and leaks it. Lookups now read the game's resource registry and
  create nothing.
- **`tuning` read a `settings.txt` that no longer shipped** (1.0.1 renamed it
  `console.txt`), so `noimmortals` and `immortalsgib` were never read.
- **Character tuning was never loaded at launch in 1.0.1 and 1.0.2.** The
  startup loader returned as soon as it found `console.txt`, before reading
  anything, so `characters.txt` applied only after a manual `tuning`. (Found by
  reading the 1.0.2 code while tracing the bug above.)
- **`up` moved the player along Y - sideways - instead of Z**, which is up in
  this engine.
- **The gold debug square was drawn whenever graphics was off.** It is meant
  only to prove SWSE owns the frame when the pipeline was asked for and failed
  to initialise; now that is the only time it appears.
- **A console line of about 240 characters crashed the game.** The print
  buffer assumed 240 bytes while the formatter writes up to 1024. The buffer
  is now bounded, and `echo` no longer overruns its own line either.
- **AI tuning could write into freed memory, and compounded.** `difficulty off`
  after a level change wrote the shipped values back to the previous level's
  addresses. Re-applying threw away every stored original, even for objects
  that survived the load, so the next capture took the already-tuned numbers
  as "shipped": each re-apply compounded, and `off` restored the tuned values.
  Each stored original now remembers which prefs record it belongs to (vtable
  and path hash, checked against the resource registry). It is used only while
  that record is still there, and is kept for as long as it is. Verified live:
  re-applying leaves the values identical, and `off` lands on the shipped
  numbers.
- **The protected cast lost its protection once character tuning ran.** AI
  tuning skips townsfolk, Clakkerz and natives by their 100000 health. Once
  `characters.txt` or `noimmortals` made them mortal, their weapons got the
  difficulty profile. Types SWSE lowers from 100000 are now remembered as
  protected.
- **`spawnnpc` refuses now.** It moved a live piece of the level (a
  GeometryInst) to the player for good, passed it where the spawn routine
  expects an InstancedObjectTag, and wrote the position into NPCTag+0x30,
  which is the factory's script token (`swse/research/RE_SPAWNING.md`).
  `npcnow` and `buildtest` no longer write that field either.
- **`writepos` could save a garbage position** when reading the player
  faulted; it now refuses.
- **Only 8 AI profiles were read**, so AT3's `[Racketeer]`, appended at the
  end, was dropped for anyone with five profiles of their own. The limit is
  now 32.
- **AgentDebugMode could freeze the game's window.** The game hides its
  cursor with a loop, `while (ShowCursor(FALSE) >= 0);`. In the background
  SWSE answered every call with 0, so the loop never ended: the window thread
  spun at 100% and handled no messages. The window read as hung, and nothing
  that needed it (a menu, `continue`) could happen.
  - In the background, SWSE now answers the loop with a counter of its own,
    so it ends.
  - The real count is put right when the window gets focus back.
- **`snap` did nothing with graphics off.** The screenshot was taken inside
  the graphics pipeline, so with the pipeline off no request was ever
  serviced. The frame hook now services it whenever the pipeline is not
  running.

### Hardened before release

Found in the 1.1 code review, and fixed before release:

- **`features <name> on|off` saves even when the system is already in that
  state.** Before, `features console off` followed by `features console on`
  answered "already on" and left `console = off` in the file: no console and
  no mailbox at the next launch.
- **`features foliage off` restores the plants' programs at once.** It used to
  queue the restore for a frame hook that stops running when foliage is off.
- **`features triggers on` in the middle of a level no longer fires every
  `levelload` trigger.** With the stock SWSE Ambushes that meant `reserve
  park`. A level that loaded less than 4 s earlier still gets its
  `levelload`.
- **The level watcher no longer treats a single missed poll as a new level.**
  The player must be gone for 1.5 s. Before, one blip re-ran every per-level
  applier and made player tuning re-capture its own tuned values as the game's.
- **Changing form is not a new level either.** `steef` and `stranger` swap
  the player's current motion. The watcher used to take that swap for a new
  level, and so re-ran every per-level applier. It now follows the player
  and its two form motions, which a swap leaves in place.
- **A teleport that does not stick is reported.** Each teleport is checked
  half a second later, and the console says if the player is not where they
  were sent (QA Q33). This check found the teleport bug (see "Teleport
  commands now move the player" above). With the engine's teleport it should
  stay silent; it stays as the independent check.
- **Nesting limits.** `exec`, `repeat`, aliases and scripts share an 8-deep
  limit, and one line may expand to at most 2,000 commands. A file that execs
  itself, or `repeat 50 repeat 50 repeat 50 ...`, stops instead of crashing or
  freezing the game. A nested `exec` no longer overwrites the outer file's
  lines.
- **Crashes on large or odd input fixed:**
  - `playertune` after `hp 99999999`;
  - `yaw 1e30`;
  - `pos` after `tpxyz 1e30 ...`;
  - over-long names reaching the prefs editor from a mod's `prefs.txt`;
  - a `nan` or `inf` yaw, which is now rejected everywhere.
- **The prefs editor refuses struct, vector and string-handle fields** unless
  you name a type (`m_x:i`). A value must be a whole number token: `2DFD1072`
  is no longer read as 2 by a float field. `010` means ten, `0x` marks hex,
  and a comma works as a decimal point. `keep` (and `knockback ... keep`,
  `npc`/`wpn ... keep`) only saves a value the editor accepted:
  `knockback thudslug keep` used to save the word `keep`.
- **`prefs restore` waits for a background apply that is still running**, so
  nothing is written after the restore.
- **`writepos` without a readable facing removes that label's older lines from
  `sites.txt`.** Otherwise the old facing line kept winning, in SWSE and in
  AT3.
- **The mailbox runs a request only once it is completely written:** the file
  ends with a newline, or has sat unchanged for a poll. A request read
  mid-write used to run twice. Requests may be 256 lines, and the reply says
  if any were cut.
- **`features.txt`, `binds.txt` and `aliases.txt` keep your comments.** A
  changed switch keeps its line's indentation and trailing comment, and there
  is no longer an 8 KB limit. The files are written beside the original, then
  swapped in, so a crash cannot leave one empty.
- **The NPC list** that `killed`/`cleared` triggers count is built on a
  background thread, not in 15 ms slices on the render thread. It is rescanned
  every 20 s while a trigger needs it, so NPCs the game creates mid-level (gib
  spawns, spawn-pool refills) are counted too.
- **Numbers written with a decimal comma** (`reload = 0,667`), which AT3 writes
  on French or German Windows, are read correctly in `aiprefs.txt`,
  `playerprefs.txt` and prefs values. They used to read as 0: instant reloads
  and perfect aim.
- **Key binds:** holding Shift alone falls back to the plain bind, so a key
  bound as `F5` also works while Shift is held. `f1x` is no longer taken as
  F1. Alias names over 31 characters are refused (they could never be run or
  removed).
- **`set` refuses a graphics key SWSE does not read, or a value that is not
  a number, and writes nothing.** A typo (`set rtao 1`) used to be written
  into `graphics.txt` and reported as set. A decimal comma is read as a
  point.
- **"No script context" says what is true.** The messages told you to pick
  up ammo to prime the console; SWSE finds the context itself once a save is
  loaded, and `autoprime` searches again at once. An unknown word typed at
  the main menu now answers "unknown", not "no context".
- **`wind save` keeps every setting and the file's notes.** It never wrote
  `sprintspeed` or `pushsprint`, so a saved file lost a custom sprint push,
  and it rewrote `wind.txt` in place. It now writes beside the file and swaps
  it in, keeping every `#` line.
- **`snap` takes a path with spaces.** It used only the first word, so a
  path under `...\New folder\` saved to `...\New`.
- **Without `wind.txt`, wind uses the shipped settings.** The built-in
  `weight`, `push` and `pushmax` were 0.5, 0.35 and 1.0, and `pushmax 1.0`
  stopped the push on grass. They now match the shipped file.
- **`auto` notices when `aiprefs.txt` stops asking.** Re-reading the file
  (`aitune`, `mods reload`, `features aituning auto`) kept the last `active`
  profile it had seen when the line, or the whole file, was gone, so AI
  tuning stayed on. A missing line now means `active = off`.
- **`features graphics off` switches ray tracing off too.** It runs inside
  the graphics pipeline and cannot run without it, but it went on reading
  `on` with nothing tracing. It goes off for the session, as a refused start
  does: `features.txt` keeps its line.
- **`features <name> on` says `refused`** when a switch will not come on
  (ray tracing without graphics, safe mode). It printed `ON` ahead of the
  reason, with the switch left off.
- **`wind seed` no longer warns that the seed makes plants twist.** The
  seed is per plant, and the twisting once blamed on it was the wrong up
  axis (wind.cpp's own finding); it says so if the axis is `y`.
- **`tools/swse_hash.py`'s `path_hash` upper-cases, as the game does.** It
  used the beta's lower-case fold, which matches none of the HD game's path
  hashes. That form is kept as `beta_path_hash`, and the script now checks
  itself against five hashes the game produced.

### What we learned from Stranger: Armed to the Teeth

Credit to **Racewizard**, whose AT3 edits the same data on disk. Each item is
verified live in the game or by disassembly, except the last, which says so;
the details are in
[SWSE_FEATURES.md](SWSE_FEATURES.md#what-we-learned-from-stranger-armed-to-the-teeth-at3).

- The player's facing is the rotation of its geometry world frame
  (`*(player+0x14) + 0x30`), not a player or motion field; the engine's setter
  `Actor::SetFacing` (RVA `0x1FC80`) is a pure store nothing rewrites per frame.
  SWSE reports it as the matrix angle `atan2(m3, m0)`, the angle AT3 and the
  level records use.
- Prefs objects carry their own path hash at `+0x0C` (verified on `NPCPrefs`,
  `NPCWeaponPrefs`, `BoltDamagePrefs`) - how the prefs editor finds any loaded
  record by path.
- Ammo types are `BoltDamagePrefs` (and sibling) records at
  `/data/prefs/weapons/<name>.txt`: `m_maxKnockSpeed` at runtime `+0x140`,
  `m_maxKnockSpeedPlayer` at `+0x144`, where `-1` means the same as for NPCs
  and `0` never knocks the player. AT3 found them at record+164 / +168 in the
  serialized records - the layouts differ. All 31 vanilla values read live
  match AT3's `AmmoKnockback.csv`.
- `FIELD_OFFSETS.tsv` filed those two fields under `BoltSurfaceSndPrefs`; the
  live class is `BoltDamagePrefs`. Its class names were noisy, its offsets
  right - which led to the exact table above.
- `m_missTime` is stored in seconds, 0.010 on almost every NPC weapon, and
  raising it makes enemies nearly unable to hit - why SWSE now applies it.
- The game's path hash (RVA `0x24D920`) is a reflected CRC-32 (`0xEDB88320`,
  init `0xFFFFFFFF`, no final xor) over the upper-cased path with `/` as `\`,
  with the low byte of the length folded in last (AT3's `gamehash.py`).
- The `.lvl` object-record chain: token `0x7A60600D`, absolute next pointer,
  class-hash marker `0x000B4265`, zone index `+20`, rotation `+25`, translation
  `+61`, uniform scale `+73`; an object appears only if its zone matches where
  it stands and its geometry's zone bundle is resident. Reference only for SWSE.
- Everything else AT3 found is catalogued against live offsets, with proposed
  commands, in `swse/research/AT3_DISCOVERIES.md` - static analysis, not yet
  run in the game. Its headlines: NPCPrefs embeds its Actor, Motion and AI
  prefs; the engine does create NPCs at runtime (on a gib, and to refill spawn
  pools); `npcaff`'s field is an ammo-immunity rule rather than an affiliation;
  and `m_species` may be where NPC hostility lives.

### Known issues

- Changing armour while Steef counts as a new level for SWSE's per-level
  settings (with player tuning on, a heal; `levelload` triggers fire again).

## 1.0.2 - 2026-07-30

Wind and foliage fixes, for the geometry corruption some players saw with
foliage wind on - stretched polygons and flickering near the player, indoors
included.

- The foliage push resolves each plant's true world-space position on every
  supported vertex program (previously 2 of 57), which removes the corruption.
- Wind is never injected into skinned character programs, so character models
  cannot be affected.
- The push radius widens while sprinting (4.5 -> 7.5) and settles back when you
  stop; retuned defaults (strength 0.075, push radius 4.5).
- `wind` reports how many character programs were refused; `wind dump` writes
  the live injected programs to a file.
- Self-test lines for wind and the character safeguard.

## 1.0.1 - 2026-07-29

- **The game no longer needs `dinput8_real.dll`.** When it was missing the game
  closed instantly with no window, while still writing `swse_log.txt`. SWSE now
  falls back to the system `dinput8.dll`; `SWSE-install.bat` became a
  convenience.
- **Mod registry:** any folder under `SWSEMods` can provide SWSE's files
  (`foliage.txt`, `aiprefs.txt`, `textures\`...), with `load_order.txt`
  deciding order and which folders are on.
- **Named positions** (`writepos`, `positions`, `goto`, `spawnat`) and
  `reserve`.
- **Triggers** - mod-defined events from `triggers.txt`, the `triggers`
  command and switch, and the `SWSE Ambushes` example mod.
- The two `settings.txt` files were renamed `SWSE Console\console.txt` and
  `SWSE Graphics\graphics.txt`: sharing one name under the registry's
  last-wins rule, the graphics pipeline was handed the console's file.

## 1.0 - 2026-07-28

The first modding framework for Oddworld: Stranger's Wrath HD.

- Byte-identical `.smb` archive repacking, validated against all 1,222
  archives in the game.
- Mod Loader: desktop GUI for texture replacement, stat editing, and mod
  management with load order.
- Script extender DLL (`dinput8` proxy): graphics pipeline with RTGI, in-game
  console with access to 181 of the game's own script functions, HD texture
  replacement (960 textures at 2x), foliage wind, additive hit reactions, and
  per-character NPC AI tuning.
- Per-system feature switches in `SWSEMods\features.txt`.
- Everything reversible; no game assets distributed.
