"""Where is Stranger's Wrath installed? The Python half of tools/swse_paths.ps1.

One answer for every SWSE tool, Steam or GOG, instead of each hard-coding the
Steam folder. Same search order and the same rule for what counts as a game:

  1. %SWSE_GAME_DIR% - set it to force a particular copy
  2. every Steam library (the registry's Steam path, then libraryfolders.vdf),
     folder "steamapps\\common\\Stranger's Wrath"
  3. GOG: registry entries whose gameName mentions Stranger, then the usual
     GOG Galaxy and standalone-installer folders

A folder counts only if stranger.exe is in it or in its bin\\ subfolder: the
Steam build keeps the exe in bin\\, a GOG build may not, so callers use
GameInstall.bin rather than assuming "bin".

    from oddforge.gamepaths import find_game
    g = find_game()            # first install found, or None
    g.root, g.bin, g.exe, g.mods, g.store
    find_game(all_=True)       # every install found
"""
from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

STEAM_DEFAULT_ROOT = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath")
GAME_FOLDER_NAME = "Stranger's Wrath"


@dataclass(frozen=True)
class GameInstall:
    store: str          # "SWSE_GAME_DIR", "Steam", "GOG", "chosen"
    root: Path          # the game folder (holds data\ and, on Steam, bin\)
    bin: Path           # the folder stranger.exe is in
    exe: Path

    @property
    def mods(self) -> Path:
        return self.root / "SWSEMods"


def check_game_dir(root: str | os.PathLike | None, store: str = "chosen") -> GameInstall | None:
    """A GameInstall if stranger.exe is in `root` or in its bin\\, else None."""
    if not root:
        return None
    r = Path(root)
    try:
        if not r.is_dir():
            return None
        for exe_dir in (r / "bin", r):
            exe = exe_dir / "stranger.exe"
            if exe.is_file():
                return GameInstall(store=store, root=r.resolve(), bin=exe_dir.resolve(),
                                   exe=exe.resolve())
    except OSError:
        return None
    return None


def _reg_value(root_key, subkey: str, name: str) -> str | None:
    try:
        import winreg
    except ImportError:                     # not Windows: no registry
        return None
    try:
        with winreg.OpenKey(root_key, subkey) as k:
            v, _ = winreg.QueryValueEx(k, name)
            return str(v) if v else None
    except OSError:
        return None


def steam_libraries() -> list[Path]:
    """Steam's own folder, then every library in libraryfolders.vdf."""
    try:
        import winreg
    except ImportError:
        return []
    steam = None
    for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                      (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam"),
                      (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam")):
        v = _reg_value(hive, key, "SteamPath") or _reg_value(hive, key, "InstallPath")
        if v:
            steam = v.replace("/", "\\")
            break
    libs: list[Path] = []
    if steam:
        libs.append(Path(steam))
        vdf = Path(steam) / "steamapps" / "libraryfolders.vdf"
        try:
            text = vdf.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        for m in re.finditer(r'"path"\s+"([^"]+)"', text):
            libs.append(Path(m.group(1).replace("\\\\", "\\")))
    seen, out = set(), []
    for p in libs:
        k = str(p).lower()
        if k not in seen:
            seen.add(k)
            out.append(p)
    return out


def _gog_registry_paths() -> list[Path]:
    try:
        import winreg
    except ImportError:
        return []
    out: list[Path] = []
    for key in (r"SOFTWARE\WOW6432Node\GOG.com\Games", r"SOFTWARE\GOG.com\Games"):
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
                i = 0
                while True:
                    try:
                        sub = winreg.EnumKey(k, i)
                    except OSError:
                        break
                    i += 1
                    name = _reg_value(winreg.HKEY_LOCAL_MACHINE, key + "\\" + sub, "gameName") or ""
                    path = _reg_value(winreg.HKEY_LOCAL_MACHINE, key + "\\" + sub, "path")
                    if "stranger" in name.lower() and path:
                        out.append(Path(path))
        except OSError:
            continue
    return out


def _gog_folder_candidates() -> list[Path]:
    pf86 = os.environ.get("ProgramFiles(x86)", "")
    pf = os.environ.get("ProgramFiles", "")
    roots = [Path(pf86) / "GOG Galaxy" / "Games" if pf86 else None,
             Path(pf) / "GOG Galaxy" / "Games" if pf else None,
             Path(r"C:\GOG Games"),
             Path(pf86) / "GOG.com" if pf86 else None,
             Path(pf) / "GOG.com" if pf else None]
    out: list[Path] = []
    for r in roots:
        if not r:
            continue
        try:
            if not r.is_dir():
                continue
            for sub in sorted(r.iterdir()):
                if sub.is_dir() and "stranger" in sub.name.lower():
                    out.append(sub)
        except OSError:
            continue
    return out


def find_game(all_: bool = False) -> GameInstall | list[GameInstall] | None:
    """The first install found (or None), or with all_=True every one."""
    found: list[GameInstall] = []

    def add(hit: GameInstall | None) -> None:
        if hit and not any(str(f.root).lower() == str(hit.root).lower() for f in found):
            found.append(hit)

    add(check_game_dir(os.environ.get("SWSE_GAME_DIR"), "SWSE_GAME_DIR"))
    if found and not all_:
        return found[0]
    for lib in steam_libraries():
        add(check_game_dir(lib / "steamapps" / "common" / GAME_FOLDER_NAME, "Steam"))
    if found and not all_:
        return found[0]
    for p in _gog_registry_paths():
        add(check_game_dir(p, "GOG"))
    for p in _gog_folder_candidates():
        add(check_game_dir(p, "GOG"))
    if all_:
        return found
    return found[0] if found else None


if __name__ == "__main__":
    games = find_game(all_=True)
    if not games:
        print("Stranger's Wrath not found - set SWSE_GAME_DIR to the game folder")
        sys.exit(1)
    for g in games:
        print(f"{g.store:14} {g.root}  (exe {g.exe})")
