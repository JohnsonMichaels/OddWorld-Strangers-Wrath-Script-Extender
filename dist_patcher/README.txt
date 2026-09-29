SWSE 4GB Patcher - Oddworld: Stranger's Wrath HD
====================================================

WHAT IT DOES
    Sets one bit in stranger.exe's PE header: IMAGE_FILE_LARGE_ADDRESS_AWARE.
    Windows then gives the game the full 4 GB of user address space instead of
    the default 2 GB.

WHAT IT DOES NOT DO
    It does NOT make the game 64-bit. The process stays 32-bit and 4 GB is a
    hard ceiling - that is all a 32-bit pointer can address. It does not make
    the game faster on its own.

WHY YOU MIGHT NEED IT
    Measured on a stock install, in a busy scene with vanilla textures,
    stranger.exe was already using 1,686 MB of its 2,048 MB address space -
    82% full, with only ~360 MB to spare.

    Doubling texture dimensions costs 4x the texture memory. An HD texture
    pack therefore does not fit in the remaining headroom, and the game will
    run out of address space and crash on level load.

    If you are NOT installing HD textures, you probably do not need this.
    The stock game runs fine within 2 GB.

USAGE
    Double-click                 patch the game it finds: the usual Steam
                                 folders on C:, D: and E:, or the game it
                                 sits in (in the game folder, in bin, or in
                                 a folder beside bin)
    SWSE_4GB_Patcher --check     report status only, change nothing
    SWSE_4GB_Patcher --restore   put the original exe back
    SWSE_4GB_Patcher --exe "<path to stranger.exe>"

    You can also drag stranger.exe onto the executable.

    If the game is under Program Files you may need to right-click ->
    "Run as administrator", otherwise the write is denied.

SAFETY
    * Before patching, a copy is saved as stranger.exe.vanilla-backup.
      That backup is never overwritten, so --restore always returns the
      original file.
    * The patch is verified after writing. If the flag did not stick, the
      backup is restored automatically.
    * Steam's "Verify integrity of game files" also restores the original -
      which means a Steam verification will UNDO this patch. Just re-run it.
    * Close the game before patching.

KNOWN RISK
    Some older programs do signed arithmetic on pointers, or store flags in
    the top bit of a pointer. With 4 GB enabled, allocations can land above
    0x80000000 where that bit is set, and such code misbehaves. Many games of
    this era take the patch without trouble; a few do not. If the game becomes
    unstable after patching, run --restore and you are back exactly where you
    started.

NOTE FOR SWSE USERS
    SWSE finds game objects by scanning the game's heap. For this patch the
    scanned range was raised (HEAP_HI, from 0x20000000 to 0x40000000), so NPC
    scanning, tuning, spawning and the camera scan keep finding objects when
    the patched game's heap grows past its old ceiling. Every SWSE release,
    1.0 and later, has this.

    Verify after patching by opening the console and running:  npcs
    If it still reports a sensible NPC count, the tooling survived.
