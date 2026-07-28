================================================================
  SWSE Console
================================================================

Press the  `  /  ~  key in-game to open the console.
Type a command and press Enter. Esc closes it. PageUp/PageDown scroll.

WORKING NOW (drives the SWSE Graphics / SWSE layer):
  help                 list all commands
  clear                clear the console
  echo <text>          print text
  ver                  version
  gfx on|off|toggle    turn the post-process effect on/off
  gfx reload           reload settings.txt live
  set <key> <value>    live-tune any graphics setting
                       e.g.  set bloom_intensity 0.6
                             set ssgi_enable 1
                             set vignette 0.2

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
  kill                 kill current target/self
  steef                transform into Steef
  stranger             transform back to Stranger
  naked                Steef naked toggle
  fps / nofps          force first- / third-person view
  sniper               force sniper view
  artifact             give artifact
  artifacts            take all artifacts
  money [amount]       set moolah (default 10000)
  tphome / tpreset     teleport home / reset
  save / checkpoint    quick save / set checkpoint
  loadsave             load last save
  healthbars           show enemy health bars
  weaponhud            open the weapon HUD

  (flycam and the dev debug buttons route through the input system, not the
   script VM - a separate bridge, coming later.)

  These call the game's OWN native script handlers (reverse-engineered from
  stranger.exe). To do that safely, SWSE must first capture a live "script
  context" the game passes to those handlers - this now happens
  AUTOMATICALLY within a second of loading a save (the HUD polling your
  health/stamina/moolah triggers it - no ammo pickup needed anymore). If a
  command ever says "faulted", the context went stale (e.g. after a level
  load) - it'll auto-reprime itself within a second.

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
Lines starting with # are comments.

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

Note: while the console is open, game keys still work (input passthrough) -
be mindful the character may move as you type until input-capture lands.
