"""Install, update and uninstall SWSE into a game folder - SWSE Setup
(swse_setup.py, and the Mod Loader's first tab), testable without either.

The source is an unzipped SWSE release: a folder holding bin\\dinput8.dll and
SWSEMods\\ (SWSE Setup.exe runs from that folder). The rules:

  * bin\\dinput8.dll goes next to stranger.exe. A dinput8.dll that is not
    SWSE's (another mod's proxy) is moved into the backup folder first.
  * SWSEMods: every shipped file is copied in, except
      - the user's own files, which are never overwritten: binds, aliases,
        sites, prefs, playerprefs and positions in SWSE Console (and the
        mailbox files), features.txt and load_order.txt - features.txt is
        then rewritten from the switches chosen, keeping its comments;
      - aiprefs.txt when it differs from the shipped one: Stranger: Armed to
        the Teeth writes its rules there, so the user's copy stays;
      - any other shipped file that differs from the new one is replaced, and
        the old copy is moved to SWSE-backup-<date>\\ in the game folder first.
    Nothing outside the shipped files is touched: a mod folder the user
    added, HD textures, logs.
  * Uninstall moves bin\\dinput8.dll (and dinput8_real.dll, from the 1.0
    helper) and SWSEMods into SWSE-uninstalled-<date>\\ in the game folder.
    Nothing is ever deleted.
  * All of it refuses while this game folder's stranger.exe runs: Windows
    keeps the loaded DLL locked.
"""
from __future__ import annotations

import filecmp
import json
import shutil
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import swsefeatures as sf
from .gamepaths import GameInstall

# The user's own files in SWSE Console: never overwritten (package_swse.ps1
# refuses to ship them for the same reason).
USER_STATE_CONSOLE = {"binds.txt", "aliases.txt", "sites.txt", "prefs.txt",
                      "playerprefs.txt", "positions.txt", "remote_in.txt", "remote_out.txt"}
# SWSEMods root files that are the user's once installed.
USER_STATE_ROOT = {"features.txt", "load_order.txt"}
# Kept when it differs from the shipped one, and said so.
KEEP_IF_DIFFERENT = {("swse console", "aiprefs.txt"):
                     "your aiprefs.txt differs from the shipped one (Stranger: Armed to the "
                     "Teeth writes its rules there) - kept yours"}


def stamp() -> str:
    return time.strftime("%Y%m%d-%H%M%S")


def find_source(start: Path) -> Path | None:
    """The unzipped release: `start`, its release\\ subfolder or its parent -
    the first holding bin\\dinput8.dll and SWSEMods\\features.txt."""
    start = Path(start)
    for cand in (start, start / "release", start.parent):
        if (cand / "bin" / "dinput8.dll").is_file() and (cand / "SWSEMods" / "features.txt").is_file():
            return cand
    return None


def default_source() -> Path | None:
    """Where SWSE Setup runs from: the exe's folder once built, the
    checkout's release\\ from source."""
    if getattr(sys, "frozen", False):
        return find_source(Path(sys.executable).resolve().parent)
    return find_source(Path(__file__).resolve().parent.parent)


@dataclass
class Report:
    lines: list[str] = field(default_factory=list)
    ok: bool = True

    def say(self, s: str) -> None:
        self.lines.append(s)

    def fail(self, s: str) -> "Report":
        self.ok = False
        self.lines.append(s)
        return self


def installed_version(game: GameInstall) -> tuple[str, dict | None]:
    """("SWSE 1.1", info) / ("not installed", None) / ("another dinput8.dll", info)."""
    dll = game.bin / "dinput8.dll"
    if not dll.is_file():
        return "not installed", None
    info = sf.version_info(dll)
    if sf.is_swse_dll(info):
        return f"SWSE {info.get('ProductVersion', '?')}", info
    if sf.is_swse_file(dll):
        return "SWSE 1.0.x (no version stamp)", info
    return "another dinput8.dll (not SWSE)", info


def _is_user_state(rel: Path) -> bool:
    parts = [p.lower() for p in rel.parts]
    if len(parts) == 1 and parts[0] in USER_STATE_ROOT:
        return True
    return len(parts) == 2 and parts[0] == "swse console" and parts[1] in USER_STATE_CONSOLE


def plan_mods(source: Path, game: GameInstall) -> list[tuple[str, Path, Path, str]]:
    """What an install would do with each shipped SWSEMods file:
    (action, src, dst, note), action one of copy / same / replace / keep."""
    src_root = source / "SWSEMods"
    dst_root = game.mods
    out: list[tuple[str, Path, Path, str]] = []
    for src in sorted(p for p in src_root.rglob("*") if p.is_file()):
        rel = src.relative_to(src_root)
        dst = dst_root / rel
        if not dst.exists():
            out.append(("copy", src, dst, ""))
            continue
        if filecmp.cmp(src, dst, shallow=False):
            out.append(("same", src, dst, ""))
            continue
        if _is_user_state(rel):
            out.append(("keep", src, dst, "yours - never overwritten"))
            continue
        key = tuple(p.lower() for p in rel.parts)
        if key in KEEP_IF_DIFFERENT:
            out.append(("keep", src, dst, KEEP_IF_DIFFERENT[key]))
            continue
        out.append(("replace", src, dst, ""))
    return out


def install(source: Path, game: GameInstall, states: dict[str, str],
            plugin_descs: dict[str, str] | None = None) -> Report:
    """Copy the release in, then write features.txt from `states`."""
    rep = Report()
    if sf.game_running(game.exe):
        return rep.fail("The game is running - close it first: Windows keeps SWSE's DLL locked.")
    src_dll = source / "bin" / "dinput8.dll"
    if not src_dll.is_file() or not (source / "SWSEMods").is_dir():
        return rep.fail(f"No SWSE release in {source} (it needs bin\\dinput8.dll and SWSEMods\\).")
    backup = game.root / f"SWSE-backup-{stamp()}"

    def back_up(p: Path, rel: Path) -> None:
        dest = backup / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(p), str(dest))

    # ---- the DLL
    dst_dll = game.bin / "dinput8.dll"
    if dst_dll.is_file() and not sf.is_swse_file(dst_dll):
        back_up(dst_dll, Path("bin") / "dinput8.dll")
        rep.say(f"another mod's dinput8.dll was in the way - moved to {backup.name}\\bin")
    if dst_dll.is_file() and filecmp.cmp(src_dll, dst_dll, shallow=False):
        rep.say("bin\\dinput8.dll: already this version")
    else:
        shutil.copy2(src_dll, dst_dll)
        v = (sf.version_info(dst_dll) or {}).get("ProductVersion", "")
        rep.say("bin\\dinput8.dll: installed" + (f" - SWSE {v}" if v else ""))

    # ---- SWSEMods
    counts = {"copy": 0, "same": 0, "replace": 0, "keep": 0}
    for action, src, dst, note in plan_mods(source, game):
        counts[action] += 1
        rel = dst.relative_to(game.root)
        if action == "copy":
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        elif action == "replace":
            back_up(dst, rel)
            shutil.copy2(src, dst)
            rep.say(f"{rel}: updated - your old copy is in {backup.name}\\")
        elif action == "keep":
            rep.say(f"{rel}: {note}")
    rep.say(f"SWSEMods: {counts['copy']} new, {counts['replace']} updated, "
            f"{counts['same']} already current, {counts['keep']} kept")

    # ---- the switches
    feats = game.mods / "features.txt"
    sf.save(feats, states, plugin_descs)
    on = [n for n, v in states.items() if v == "on"]
    auto = [n for n, v in states.items() if v == "auto"]
    rep.say("features.txt: on - " + (", ".join(on) or "nothing")
            + (("; auto - " + ", ".join(auto)) if auto else ""))
    return rep


def uninstall(game: GameInstall) -> Report:
    """Move SWSE out of the game folder; never delete."""
    rep = Report()
    if sf.game_running(game.exe):
        return rep.fail("The game is running - close it first: Windows keeps SWSE's DLL locked.")
    applied = game.mods / ".installed.json"
    try:
        archives = json.loads(applied.read_text()).get("archives", []) if applied.is_file() else []
    except (OSError, ValueError):
        archives = []
    if archives:
        return rep.fail("Archive mods are applied to the game's data (by the Mod Loader or "
                        "tools/mods_cli.py). Revert them there first (\"Revert to Vanilla\"): "
                        "their backups live in SWSEMods.")
    dest = game.root / f"SWSE-uninstalled-{stamp()}"
    moved = []
    for name in ("dinput8.dll", "dinput8_real.dll"):
        p = game.bin / name
        if not p.is_file():
            continue
        if name == "dinput8.dll" and not sf.is_swse_file(p):
            rep.say("bin\\dinput8.dll is not SWSE's - left where it is")
            continue
        (dest / "bin").mkdir(parents=True, exist_ok=True)
        shutil.move(str(p), str(dest / "bin" / name))
        moved.append(f"bin\\{name}")
    if game.mods.is_dir():
        dest.mkdir(parents=True, exist_ok=True)
        shutil.move(str(game.mods), str(dest / "SWSEMods"))
        moved.append("SWSEMods")
    if not moved:
        return rep.fail("Nothing to uninstall: no SWSE dinput8.dll and no SWSEMods folder.")
    rep.say(f"moved {', '.join(moved)} to {dest.name}\\ - the game is vanilla again; "
            "delete that folder yourself once you are sure")
    return rep


def hd_textures(game: GameInstall) -> int:
    """How many .oft files the HD pack's folder holds."""
    d = game.mods / "SWSE HD" / "textures"
    try:
        return sum(1 for p in d.rglob("*") if p.suffix.lower() == ".oft")
    except OSError:
        return 0


def large_address_aware(exe: Path) -> bool | None:
    """Has the 4GB patch been applied (IMAGE_FILE_LARGE_ADDRESS_AWARE)? None if unreadable."""
    try:
        with open(exe, "rb") as f:
            head = f.read(4096)
        pe = int.from_bytes(head[0x3C:0x40], "little")
        if head[pe:pe + 4] != b"PE\0\0":
            return None
        chars = int.from_bytes(head[pe + 22:pe + 24], "little")
        return bool(chars & 0x0020)
    except (OSError, ValueError):
        return None
