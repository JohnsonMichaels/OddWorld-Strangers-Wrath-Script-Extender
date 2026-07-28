# The "Stranger gets stuck" bug - frame-rate dependent movement

The most-cited defect in the HD PC port. Stranger locks in place when he stops
moving; jumping or pausing/unpausing frees him. The community has no fix, only
workarounds: enable VSync, or lower `maxfps` (defaults to 100) in
`Documents\Oddworld\Stranger's Wrath HD\config.txt`. People cap their frame
rate on modern hardware to keep the game playable.

Sources: [PCGamingWiki](https://www.pcgamingwiki.com/wiki/Oddworld:_Stranger%27s_Wrath_HD),
Steam threads "Stuck in place, unable to move" and "MOVEMENT BUG".

## The timing globals - FOUND

**Note the base.** `stranger.exe` is ASLR'd (base was `0xB0000` in one run, not
`0x400000`), and older notes in this repo quote addresses from static analysis
at the default base. Subtract `0x400000` from those to get an RVA. The game
time referred to as `0x9D5540` in scriptvm.cpp is therefore **RVA `0x5D5540`**.

Measured live, dumping that neighbourhood:

```
RVA 0x5D5540   double  game time, seconds
RVA 0x5D5548   float   game time, copy
RVA 0x5D554C   float   game time, copy
RVA 0x5D5550   float   FRAME DELTA          <- the one that matters
RVA 0x5D5554   float   frame delta, copy
```

Verified rather than assumed: over ~4.5 s of wall time the game time advanced
233.346 -> 237.846, while the delta held steady at ~0.0166 (1/60, matching the
`vsync` line in config.txt).

## Why this is the foothold

If movement is frame-rate dependent, it is because something consumes that
delta. Having it live means we can WATCH it alongside the player's motion state
instead of theorising: reproduce the stick with VSync off, and log the delta,
the player's velocity and the MotionImpl fields together.

## What NOT to do

**Do not simply clamp the delta upward.** Forcing a minimum step makes the
simulation advance more game time than real time, and the game runs fast. The
delta is an observation of how long the frame took, not a knob.

The plausible mechanisms, to be distinguished by measurement:

* a threshold/epsilon in the "am I moving" test that a tiny per-frame
  displacement fails to exceed - the stick-on-stop symptom fits this well
* a friction or stiction term scaled wrongly by the delta
* an integration that loses precision at very small steps

Only the first would be fixed by adjusting a constant; the others need the
update itself substepped.

## Known footholds

* `MotionImpl` / `MotionImplDummy` vtables at RVA `0x37A924` / `0x37AC0C`; the
  player OWNS its motion objects at `player+0xB0` and `player+0x1A4`, so no
  scanning is needed (see scriptvm.cpp).
* Fields already mapped: `m_jumpHeightMax` +0x18, `m_overVel_Walk` +0x78,
  `m_overVel_Run` +0x84; `GlobalMotion` has `m_gravityForFall` +0x68 and
  air control at +0x20/+0x24.
* `quicksave=F1` / `quickload=F5` in config.txt make repro cycles cheap.

## REPRODUCED, and it is an INPUT bug, not physics

Reproduced deliberately: unchecking VSync in the launcher (the launcher
REWRITES config.txt from its own UI on every launch, so editing the file alone
achieves nothing) gave **193 fps** and the frame delta dropped to **0.005**.
The defect was immediately obvious in play - "terrible", "annoying" - and it
presents as input dropping out rather than the character being physically
stuck.

**The game uses RAW INPUT.** Checked against the executable's imports:

```
RegisterRawInputDevices    present
GetRawInputData            present
DirectInput8Create         present
GetAsyncKeyState           -
GetKeyboardState           -
GetCursorPos / SetCursorPos -
```

That reframes everything:

* Raw input events arrive only on state CHANGE, at the device's report rate. At
  193 fps most frames see no event at all, so anything that rebuilds key state
  per frame from events will drop input - movement stops, then resumes when the
  next event lands. An input-path bug that merely LOOKS like physics.
* SWSE's key injection cannot drive the game: `key w` moves nothing, and
  `inputst` reports 0 DirectInput reads. Any automated movement test needs the
  raw input path, not DirectInput.
* AgentDebugMode's input suppression works on DirectInput and GetAsyncKeyState,
  which the game does not use for movement - so it is unlikely to have caused
  the transient stutter it was blamed for. The focus debounce added alongside
  is still correct on its own terms, but it was not the fix it was presented as.

**Priority: low.** VSync caps the frame rate to 60 and the bug does not occur
there, which is how the machine is configured.

## Next step, if resumed

Reproduce with VSync off so the delta drops well below 1/60, then log the delta
and the motion fields together across a stick. The question to answer first is
narrow: **when Stranger is stuck, is his velocity zero, or is it non-zero but
never applied?** Those point at different fixes and are trivial to tell apart
once both are on screen.
