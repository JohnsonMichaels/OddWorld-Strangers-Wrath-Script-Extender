================================================================
  SWSE Console  (SWSE 1.1.1)
================================================================

Press the  `  /  ~  key in-game to open the console.
Type a command and press Enter. Esc closes it. PageUp/PageDown scroll.
Tab completes a command name.

NEW IN 1.1.1 - the console's new look and keys:
  * The frame rate, in fps and ms, at the top right of the console.
  * Text drawn sharp at the window's size; errors in red, results in green.
  * While you type a command name, a card under the console lists the
    commands it could be: Up/Down choose, Tab takes one, Esc hides the card
    (a second Esc closes the console).
  * Up/Down with no card showing bring back what you typed before.
  * Ctrl+V or Shift+Insert pastes. Each pasted line runs, in order (64 at
    most; lines starting with # are skipped); text after the last line
    break stays in the input line.
  * Ctrl+C copies the line you are typing - or, with nothing typed, the
    last command and its output. Ctrl+Shift+C copies the lines on screen.
  * No new commands: the same commands as 1.1.

Since SWSE 1.1 the console is the ONLY system switched on by default
(SWSE Setup.exe, in the download, switches the others with ticks). The
other SWSE systems (graphics, HD textures, wind, hit reactions, triggers,
character tuning, prefs edits, and the experimental ray tracing) are off
until you switch them on. AI tuning and player tuning are `auto`: each runs
only while its own file (below) asks for something. Switch any of them from
here:

  features                   list every system, on or off, and what it does
  features <name> on         switch one on now AND remember it
  features <name> on temp    switch one on for this session only
  features <name> off        switch one off (puts the game's values back
                             where that system keeps them)
  features preset full       the classic SWSE in one word: console, graphics,
                             hdtextures, hitreact, foliage and aituning on
  features preset default    back to the 1.1 default (console only)
  features preset list       what each preset sets (both take `temp` too)
  status                     where you are, health, what is switched on
  ver                        which SWSE this is

On a game build SWSE does not recognise (the GOG version), the console runs
in SAFE MODE: the commands that would read or change the game are refused
with a one-line message, and `help` marks them with a *. `status` says so.

CONSOLE AND GRAPHICS COMMANDS:
  help                 list all commands  ('help <category>' for details)
  clear                clear the console
  echo <text>          print text
  ver                  version
  gfx on|off|toggle    turn the post-process effect on/off
  gfx reload           reload graphics.txt live
  set <key> <value>    live-tune any graphics setting (also saved to
                       graphics.txt; an unknown key is refused)
                       e.g.  set bloom_intensity 0.6
                             set ssgi_enable 1
                             set vignette 0.2
  (gfx and set need the graphics feature:  features graphics on)

GAME-STATE COMMANDS (LIVE via the script-VM bridge):
  god                  toggle invulnerability (re-maxes health each frame)
  ammo                 give all ammo
  defaultammo          give default ammo
  noammo               take all ammo
  crossbow             give the crossbow
  noweapons            take all weapons
  heal                 restore health + stamina
  maxhealth            max health
  maxstamina           max stamina
  sethealth <n>        set health value
  kill                 the game's own kill verb (no effect on Stranger)
  steef                transform into Steef
  stranger             transform back to Stranger
  naked                Steef naked toggle
  fps / nofps          force first- / third-person view
  sniper               force sniper view
  artifact             give artifact
  artifacts            list all 53 artifacts
  money [amount]       set moolah (default 10000)
  tphome / tpreset     the game's teleport-home / reset verbs: they need a
                       script target and do nothing from the console (use
                       tp, goto or tpxyz)
  save / checkpoint    quick save / set checkpoint
  loadsave             load last save
  healthbars           show enemy health bars
  weaponhud            open the weapon HUD

  (The developers' fly camera is the `freecam` command - see below.)

  These call the game's OWN native script handlers (reverse-engineered from
  stranger.exe). To do that safely, SWSE needs a live "script context" -
  an object the game passes to those handlers. It finds one AUTOMATICALLY
  once a save is loaded: the first command that needs it searches memory
  for it, so no ammo pickup is needed. At the main menu there is none yet,
  and the command says so - load a save and retry (`autoprime` forces the
  search). If a command ever says "faulted", the context went stale (e.g.
  after a level load); the next command finds a fresh one by itself.

WHERE YOU ARE AND WHICH WAY YOU FACE (new in 1.1: facing is saved too):
  pos                  your x y z and facing (yaw in degrees - the same angle
                       the game's own level files use for placed objects)
  yaw [deg]            read or set the way you face
  savepos / tp         quick-save your spot AND facing / go back to it
                       (one slot, kept until you quit)
  savepos <label>      save it by name, to file (same as writepos <label>)
  tp <label>           go to a named spot, facing restored (same as goto)
  tpxyz <x> <y> <z> [yaw]   teleport to coordinates
  up [dist]            lift yourself dist units (default 10) and fall back down
                       (no ceiling check - use it in the open)
  Teleporting into an area where a story fight is about to start can crash
  the game (seen twice in testing): save first, and walk into those fights.
  positions            list named spots

DISCOVERY COMMANDS:
  list [filter]        search all 181 auto-exposed game functions
                        e.g. "list music"  "list boat"  "list camera"
  call <fn> [args]      invoke any of them directly by name
                        (or just type the function name - it works as a
                        command too, e.g. "EnableCombatMusic")

================================================================
  WRITE YOUR OWN COMMANDS - no C++, no rebuild
================================================================

Drop a .txt file into this mod's "scripts" folder:

    SWSEMods\SWSE Console\scripts\<yourname>.txt

The FILENAME (without .txt) becomes a new console command. Each line inside
it runs in order, exactly as if you'd typed it yourself - so a script can
chain built-ins, any of the 181 game functions, freeze/poke, anything.
Lines starting with # are comments. A line `wait 500` pauses the rest of the
script for half a second (1.1).

Example - SWSEMods\SWSE Console\scripts\moolah.txt:
    money 999999
Now typing "moolah" in-game runs that.

Example - a whole loadout in one command (scripts\loadout.txt):
    ammo
    heal
    crossbow
Now "loadout" gives you all three at once.

Two examples ship in the scripts\ folder already - open them, copy the
pattern, make your own. After adding/editing a script file, type
"reloadscripts" in-game to pick up the changes without restarting.
Type "scripts" to see everything currently loaded.

NEW IN 1.1 - play as another character in the level:
  playnpc                        list the level's characters (ready = in
                                 memory, load = streamed in when you pick it)
  playnpc <name>                 become that character (Stranger spins into
                                 it; add nospin to swap instantly)
  playnpc off                    back to Stranger
  The character brings its own health and stamina (townsfolk and natives
  are capped at Stranger's), wears its own hat and weapon, and plays its
  own death animation. Stranger's crossbow and headbutt are off while you
  play one; the right mouse button still sprints.
  A level change, death or checkpoint also turns you back. The body is never
  saved into your game.

NEW IN 1.1 - a free-flying view (the developers' own fly camera):
  freecam on / freecam off       detach the view / back to the player camera
  freecam toggle                 for a key:  bind F6 freecam toggle
  freecam pos <x> <y> <z>        put the camera somewhere
  freecam look <yaw> [pitch]     aim it
  freecam speed [n]              flying speed (default 40)
  freecam sens [n] [updown]      how far the mouse turns it (default 8; a
                                 negative updown inverts up/down)
  Close the console to fly: the move keys fly it, the mouse or the arrow keys
  turn it, and Stranger stays put meanwhile. A level change, death or
  cutscene turns it off.

NEW IN 1.1 - more ways to automate, all from the console:
  cmd1; cmd2; wait 500; cmd3     run several commands in one line
  alias <name> <commands>        a new command made of others (%1..%9 =
                                 its arguments); saved in aliases.txt
  bind <key> <command>           run a command from a key while the
                                 console is closed; saved in binds.txt
  after <sec> <command>          run a command later
  repeat <n> <command>           run it n times (up to 50)
  exec <file>                    run a text file of commands from this folder
  log <text>                     write a line into bin\swse_log.txt
  unbind / binds / unalias / aliases   remove or list them
  hide / show                    close or open this overlay (for tools)

NEW IN 1.1 - tuning from files in this folder (commented templates for
playerprefs.txt, prefs.txt, binds.txt and aliases.txt are in examples\ -
copy one up a folder to use it; the commands also create them):
  playerprefs.txt   the player's health, stamina, speed, jump, gravity and
                    air control, every level. Runs once the file sets a
                    value (playertune is auto: checked at launch, or now
                    with `features playertune auto`); `playertune` shows
                    it. Blank values change nothing.
  prefs.txt         live edits to any loaded prefs record, every level
                    (features prefsedit on; `prefs` in the console).
                    `knockback` lists and sets ammo knockback.
  aiprefs.txt       enemy AI profiles. Runs once `active =` names a profile
                    (aituning is auto: checked at launch, or now with
                    `features aituning auto`); `difficulty` picks one.
  console.txt       character rules such as noimmortals (features npctuning
  characters.txt    on; `tuning` loads them by hand).

NEW IN 1.1 - native plugins (code a mod ships in its plugins\ folder):
  plugins                        every plugin found: on or off, version, mod,
                                 frame cost ('plugins <name>' for details)
  features <plugin> on           load one and switch it on. A plugin is named
                                 after its DLL and is off - not even loaded -
                                 until you name it
  hooks                          every patch SWSE has made to the game, and
                                 which system or plugin made it
  A plugin that crashes is switched off and named; the game keeps running.
  Plugins run with the game's full rights: install them only from people you
  trust.

FOR TOOLS: SWSE reads commands from remote_in.txt in this folder and writes
the replies to remote_out.txt, so a program can drive the game without
touching the keyboard. `query player`, `query position`, `query features`,
`query plugins`, `query version` and `query contract` answer in key=value form
for them.
TOOL_CONTRACT.md in the SWSE repository lists what stays stable.

NEW IN 1.1 - menus without keys (they work with the game in the background):
  menu list                      which menu screens are up right now
  menu continue                  the main menu's CONTINUE (loads the last save)
  menu skip / menu resume        a paused movie's SKIP / PLAY
  menu fs <screen> <cmd> [arg]   any screen's own command,
                                 e.g.  menu fs Pause quick_save

Note: while the console is open, the game gets no keyboard or mouse input
(new in 1.1), so typing never walks Stranger and the mouse does not turn the
camera - the key that closes the console included. Key releases still reach
the game, so nothing is left held down. Key binds work only while the
console is closed. The console itself reads the keyboard only while the game
is the window you are in, so typing in another app never lands in it.
