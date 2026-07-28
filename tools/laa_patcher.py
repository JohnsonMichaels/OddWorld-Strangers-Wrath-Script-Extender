"""SWSE 4GB Patcher - make Stranger's Wrath large-address-aware.

WHAT IT DOES
    Sets IMAGE_FILE_LARGE_ADDRESS_AWARE (0x0020) in stranger.exe's PE header.
    That is a single bit. The game stays 32-bit; what changes is that Windows
    gives it the full 4 GB of user address space instead of 2 GB.

WHY IT IS NEEDED
    Measured in a busy scene with vanilla textures, stranger.exe was already
    using 1,686 MB of its 2,048 MB address space - 82% full, ~360 MB spare.
    Doubling texture dimensions costs 4x texture memory, which does not fit.
    Without this patch an HD texture pack will run out of address space and
    crash on level load.

WHAT IT DOES NOT DO
    It does NOT make the game 64-bit, and it does NOT make it faster. 4 GB is
    a hard ceiling: that is all a 32-bit pointer can address.

SAFETY
    A .vanilla-backup copy is written before the first patch and never
    overwritten afterwards, so --restore always returns the original file.
    Steam's "Verify integrity of game files" also restores it (and will undo
    the patch, so re-run this afterwards).

USAGE
    laa_patcher.exe                 patch the auto-detected install
    laa_patcher.exe --check         report status, change nothing
    laa_patcher.exe --restore       put the vanilla exe back
    laa_patcher.exe --exe <path>    point at a specific stranger.exe
"""
from __future__ import annotations

import shutil
import struct
import sys
from pathlib import Path

LAA_FLAG = 0x0020
BACKUP_SUFFIX = ".vanilla-backup"

SEARCH = [
    r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\bin\stranger.exe",
    r"C:\Program Files\Steam\steamapps\common\Stranger's Wrath\bin\stranger.exe",
    r"D:\Steam\steamapps\common\Stranger's Wrath\bin\stranger.exe",
    r"D:\SteamLibrary\steamapps\common\Stranger's Wrath\bin\stranger.exe",
    r"E:\SteamLibrary\steamapps\common\Stranger's Wrath\bin\stranger.exe",
]


def find_exe() -> Path | None:
    for p in SEARCH:
        if Path(p).is_file():
            return Path(p)
    # last resort: next to us, or one directory up (portable drop-in use)
    here = Path(sys.argv[0]).resolve().parent
    for cand in (here / "stranger.exe", here / "bin" / "stranger.exe",
                 here.parent / "bin" / "stranger.exe"):
        if cand.is_file():
            return cand
    return None


def read_characteristics(exe: Path) -> tuple[int, int, int, int]:
    """Return (pe_offset, characteristics, machine, opt_magic). Raises on non-PE."""
    with open(exe, "rb") as f:
        if f.read(2) != b"MZ":
            raise ValueError("not an executable (no MZ header)")
        f.seek(0x3C)
        pe_off = struct.unpack("<I", f.read(4))[0]
        f.seek(pe_off)
        if f.read(4) != b"PE\0\0":
            raise ValueError("not a PE file (bad PE signature)")
        machine = struct.unpack("<H", f.read(2))[0]
        f.seek(pe_off + 0x16)
        chars = struct.unpack("<H", f.read(2))[0]
        f.seek(pe_off + 0x18)
        magic = struct.unpack("<H", f.read(2))[0]
    return pe_off, chars, machine, magic


def describe(exe: Path) -> bool:
    """Print the file's state. Returns True if the LAA flag is set."""
    pe_off, chars, machine, magic = read_characteristics(exe)
    arch = {0x014C: "x86 (32-bit)", 0x8664: "x64 (64-bit)"}.get(machine, f"0x{machine:04X}")
    print(f"  file           : {exe}")
    print(f"  size           : {exe.stat().st_size:,} bytes")
    print(f"  machine        : {arch}")
    print(f"  PE format      : {'PE32' if magic == 0x10B else 'PE32+'}")
    print(f"  characteristics: 0x{chars:04X}")
    on = bool(chars & LAA_FLAG)
    print(f"  LARGE_ADDRESS_AWARE: {'SET - already patched (4 GB)' if on else 'not set (2 GB limit)'}")
    if machine == 0x8664:
        print("  note: this is already a 64-bit binary; the patch is meaningless here.")
    return on


def patch(exe: Path) -> int:
    pe_off, chars, machine, magic = read_characteristics(exe)
    if machine != 0x014C:
        print("\nrefusing: this is not a 32-bit x86 executable.")
        return 1
    if chars & LAA_FLAG:
        print("\nalready patched - nothing to do.")
        return 0

    backup = exe.with_suffix(exe.suffix + BACKUP_SUFFIX)
    if backup.exists():
        print(f"\nbackup already present, keeping it: {backup.name}")
    else:
        shutil.copy2(exe, backup)
        print(f"\nbackup written: {backup.name}")

    new_chars = chars | LAA_FLAG
    try:
        with open(exe, "r+b") as f:
            f.seek(pe_off + 0x16)
            f.write(struct.pack("<H", new_chars))
    except PermissionError:
        print("\nPERMISSION DENIED writing to the exe.")
        print("Close the game, and if it is under Program Files, run this as Administrator.")
        return 1

    _, verify, _, _ = read_characteristics(exe)
    if verify & LAA_FLAG:
        print(f"patched: characteristics 0x{chars:04X} -> 0x{verify:04X}")
        print("\nDone. The game can now use 4 GB instead of 2 GB.")
        print("If anything misbehaves, re-run with --restore.")
        return 0
    print("\nverification FAILED - the flag did not stick. Restoring the backup.")
    shutil.copy2(backup, exe)
    return 1


def restore(exe: Path) -> int:
    backup = exe.with_suffix(exe.suffix + BACKUP_SUFFIX)
    if not backup.exists():
        print(f"\nno backup found at {backup.name}")
        print("Use Steam -> Properties -> Installed Files -> Verify integrity instead.")
        return 1
    shutil.copy2(backup, exe)
    print(f"\nrestored the vanilla exe from {backup.name}")
    return 0


def main() -> int:
    args = [a for a in sys.argv[1:]]
    mode = "patch"
    exe_arg = None
    i = 0
    while i < len(args):
        a = args[i].lower()
        if a in ("--check", "-c"):
            mode = "check"; i += 1
        elif a in ("--restore", "-r"):
            mode = "restore"; i += 1
        elif a in ("--help", "-h", "/?"):
            print(__doc__); return 0
        elif a == "--exe" and i + 1 < len(args):
            exe_arg = Path(args[i + 1]); i += 2
        elif not a.startswith("-") and exe_arg is None:
            exe_arg = Path(args[i]); i += 1        # allow drag-and-drop
        else:
            print(f"unknown argument: {args[i]}"); return 1

    print("=" * 62)
    print(" SWSE 4GB Patcher  -  Oddworld: Stranger's Wrath HD")
    print("=" * 62)

    exe = exe_arg if exe_arg else find_exe()
    if exe is None or not exe.is_file():
        print("\nCould not find stranger.exe.")
        print("Pass its path:  laa_patcher.exe --exe \"...\\bin\\stranger.exe\"")
        print("or drop stranger.exe onto this executable.")
        return 1

    try:
        already = describe(exe)
    except ValueError as e:
        print(f"\n{exe}: {e}")
        return 1

    if mode == "check":
        return 0
    if mode == "restore":
        return restore(exe)
    if already:
        print("\nNothing to do.")
        return 0
    return patch(exe)


if __name__ == "__main__":
    code = main()
    # Double-clicked from Explorer: keep the window open so the result is readable.
    if sys.stdout.isatty() and not sys.argv[1:]:
        try:
            input("\nPress Enter to close...")
        except EOFError:
            pass
    raise SystemExit(code)
