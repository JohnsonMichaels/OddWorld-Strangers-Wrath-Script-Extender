"""SWSE Mod Loader.

A mod is a folder under SWSEMods/ containing a mod.json manifest plus any of:

    textures/<in-game path>.png     replace a texture by its game path
                                    e.g. textures/data/textures/gui/region_00.png
    values.json                     stat/value patches (see PatchSet below)
    scripts/<name>.foo              (reserved) script overrides

load_order.txt lists mod folder names, one per line, top = loaded first; later
mods win conflicts. Missing file => all mods, alphabetical.

Applying:
  1. resolve merged patch set across mods in order
  2. group patches by target archive
  3. for each archive: restore-from-backup-or-back-up, apply, rebuild, install
  4. record installed state so we can cleanly revert to vanilla

Everything is reversible: revert() copies every backed-up archive back.
"""
from __future__ import annotations

import json
import shutil
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image

from .container import SmbContainer
from .stats import find_named_blocks, find_stat_blocks, set_field
from .textures import replace_entry
from .toc import find_textures


@dataclass
class Mod:
    folder: Path
    name: str
    author: str
    version: str
    description: str
    enabled: bool = True

    @classmethod
    def load(cls, folder: Path, enabled: bool = True) -> "Mod":
        # Read the manifest as UTF-8 with replacement, and treat a broken one
        # as missing rather than fatal. mod.json files come from the community:
        # people write them in Notepad with whatever encoding their system
        # uses, and one bad byte in one mod must not kill discovery of every
        # mod. Verified the hard way - a stray 0x9d in a description crashed
        # the whole loader on a cp1252-default system.
        manifest = folder / "mod.json"
        meta = {}
        if manifest.exists():
            try:
                meta = json.loads(manifest.read_text(encoding="utf-8",
                                                     errors="replace"))
            except (json.JSONDecodeError, OSError) as e:
                print(f"  warning: bad mod.json in {folder.name}: {e}")
        return cls(folder=folder,
                   name=meta.get("name", folder.name),
                   author=meta.get("author", "unknown"),
                   version=meta.get("version", "0"),
                   description=meta.get("description", ""),
                   enabled=enabled)


@dataclass
class ModLoader:
    game_data: Path                 # ...\Stranger's Wrath\data
    mods_root: Path                 # ...\Stranger's Wrath\SWSEMods
    _tex_index: dict[str, str] | None = field(default=None, repr=False)

    # ------------------------------------------------------------------ paths
    @property
    def backups_dir(self) -> Path:
        return self.mods_root / ".backups"

    @property
    def load_order_file(self) -> Path:
        return self.mods_root / "load_order.txt"

    @property
    def installed_file(self) -> Path:
        return self.mods_root / ".installed.json"

    @property
    def index_file(self) -> Path:
        return self.mods_root / ".texture_index.json"

    # ------------------------------------------------------------------ mods
    def discover(self) -> list[Mod]:
        """All mods in load order. Disabled mods are marked '# name' in the file
        (still listed, still discoverable, just skipped at apply time)."""
        if not self.mods_root.exists():
            return []
        folders = {p.name: p for p in self.mods_root.iterdir()
                   if p.is_dir() and not p.name.startswith(".")}
        ordered_names, disabled = self._read_order_raw()
        result: list[Mod] = []
        for n in ordered_names:
            if n in folders:
                result.append(Mod.load(folders[n], enabled=n not in disabled))
        for n in sorted(folders):  # append any not yet listed (new drops), enabled
            if n not in ordered_names:
                result.append(Mod.load(folders[n], enabled=True))
        return result

    def _read_order_raw(self) -> tuple[list[str], set[str]]:
        """Return (ordered names incl. disabled, set of disabled names).

        Line syntax: 'ModName' = enabled, '!ModName' = disabled, '#...' = comment.

        Read as utf-8-sig: Notepad (and PowerShell) write a UTF-8 BOM, and
        without stripping it the first line fails the '#' comment test, so the
        header comment gets parsed as a mod name. Found in QA when the header
        itself came back from read_load_order().
        """
        if not self.load_order_file.exists():
            return [], set()
        names, disabled = [], set()
        text = self.load_order_file.read_text(encoding="utf-8-sig",
                                              errors="replace")
        for ln in text.splitlines():
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            if ln.startswith("!"):
                body = ln[1:].strip()
                if body:
                    names.append(body)
                    disabled.add(body)
            else:
                names.append(ln)
        return names, disabled

    def read_load_order(self) -> list[str]:
        names, disabled = self._read_order_raw()
        return [n for n in names if n not in disabled]

    def validate_load_order(self) -> list[str]:
        """Entries in load_order.txt that match no installed mod folder.

        A typo or a renamed folder otherwise fails silently: the flag simply
        stops binding and the mod behaves as enabled. Exactly this happened
        when the mod folders were renamed and load_order.txt still listed the
        old names - the '!' disables quietly did nothing.
        """
        names, _ = self._read_order_raw()
        installed = {f.name for f in self.mods_root.iterdir() if f.is_dir()}
        return [n for n in names if n not in installed]

    def write_load_order(self, mods: list[Mod]) -> None:
        """Persist order + enabled state. Disabled mods written as '!name'."""
        self.mods_root.mkdir(parents=True, exist_ok=True)
        header = ("# SWSE load order - top loads first; later mods win conflicts.\n"
                  "# 'ModName' = enabled, '!ModName' = disabled, '#...' = comment.\n")
        lines = [(m.folder.name if m.enabled else f"!{m.folder.name}") for m in mods]
        self.load_order_file.write_text(header + "\n".join(lines) + "\n")

    def set_enabled(self, folder_name: str, enabled: bool) -> None:
        mods = self.discover()
        for m in mods:
            if m.folder.name == folder_name:
                m.enabled = enabled
        self.write_load_order(mods)

    # ------------------------------------------------------------------ texture index
    def build_texture_index(self, force: bool = False) -> dict[str, str]:
        """Map in-game texture path (lowercased) -> archive relpath that holds it."""
        if self._tex_index is not None and not force:
            return self._tex_index
        if self.index_file.exists() and not force:
            self._tex_index = json.loads(self.index_file.read_text())
            return self._tex_index
        index: dict[str, str] = {}
        for smb in sorted(self.game_data.rglob("*.smb")):
            try:
                c = SmbContainer.parse_file(smb)
            except Exception:  # noqa: BLE001
                continue
            rel = str(smb.relative_to(self.game_data)).replace("\\", "/")
            for t in find_textures(c):
                key = t.name.lower().lstrip("\\").replace("\\", "/")
                index.setdefault(key, rel)  # first archive wins
        self.mods_root.mkdir(parents=True, exist_ok=True)
        self.index_file.write_text(json.dumps(index, indent=0))
        self._tex_index = index
        return index

    # ------------------------------------------------------------------ patch collection
    def collect_patches(self, mods: list[Mod]) -> dict[str, dict]:
        """Merge all mods' patches, grouped by target archive relpath.

        Returns {archive_rel: {"textures": {tex_key: image_path},
                               "values": [patch, ...]}}
        """
        index = self.build_texture_index()
        by_archive: dict[str, dict] = {}

        def bucket(rel: str) -> dict:
            return by_archive.setdefault(rel, {"textures": {}, "values": []})

        for mod in mods:  # order matters: later mods overwrite
            tex_dir = mod.folder / "textures"
            if tex_dir.is_dir():
                for img in tex_dir.rglob("*"):
                    if img.suffix.lower() not in (".png", ".jpg", ".jpeg", ".bmp", ".tga"):
                        continue
                    key = str(img.relative_to(tex_dir)).replace("\\", "/")
                    key = key.rsplit(".", 1)[0].lower()  # drop mod-side extension
                    # try to match against a known game texture path
                    match = self._match_texture_key(index, key)
                    if match is None:
                        print(f"  [warn] {mod.name}: no game texture matches '{key}'")
                        continue
                    game_key, rel = match
                    bucket(rel)["textures"][game_key] = img

            values_file = mod.folder / "values.json"
            if values_file.exists():
                patch = json.loads(values_file.read_text())
                for v in patch.get("values", []):
                    rel = v["archive"].replace("\\", "/")
                    bucket(rel)["values"].append(v)

        return by_archive

    @staticmethod
    def _match_texture_key(index: dict[str, str], key: str):
        # exact, or endswith (mod folder may omit leading data/ path)
        if key in index:
            return key, index[key]
        for game_key, rel in index.items():
            if game_key.rsplit(".", 1)[0] == key or game_key.rsplit(".", 1)[0].endswith("/" + key):
                return game_key, rel
        return None

    # ------------------------------------------------------------------ apply
    def _backup(self, rel: str) -> Path:
        src = self.game_data / rel
        dst = self.backups_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.exists():
            shutil.copy2(src, dst)
        return dst

    def apply(self, mods: list[Mod] | None = None) -> dict:
        """Apply all enabled mods. Idempotent: reverts to vanilla first, so the
        installed game state always matches exactly the currently enabled set."""
        if mods is None:
            mods = self.discover()
        mods = [m for m in mods if m.enabled]
        self.revert()  # start from a clean vanilla base every time
        by_archive = self.collect_patches(mods)
        summary = {"mods": [m.name for m in mods], "archives": {}, "warnings": []}

        for rel, patches in by_archive.items():
            backup = self._backup(rel)
            c = SmbContainer.parse(backup.read_bytes())  # always start from vanilla
            n_tex = n_val = 0

            if patches["textures"]:
                entries = {t.name.lower().lstrip("\\").replace("\\", "/"): t
                           for t in find_textures(c)}
                for game_key, img_path in patches["textures"].items():
                    t = entries.get(game_key)
                    if t is None or not t.is_dxt1:
                        summary["warnings"].append(f"{rel}: cannot replace {game_key}")
                        continue
                    replace_entry(c, t, Image.open(img_path))
                    n_tex += 1

            if patches["values"]:
                blocks = find_stat_blocks(c) + find_named_blocks(c)
                for v in patches["values"]:
                    if self._apply_value(c, blocks, v):
                        n_val += 1
                    else:
                        summary["warnings"].append(f"{rel}: value patch missed {v}")

            (self.game_data / rel).write_bytes(c.build())
            summary["archives"][rel] = {"textures": n_tex, "values": n_val}

        self.installed_file.parent.mkdir(parents=True, exist_ok=True)
        self.installed_file.write_text(json.dumps(
            {"mods": summary["mods"], "archives": list(summary["archives"])}, indent=2))
        return summary

    @staticmethod
    def _apply_value(c: SmbContainer, blocks, v: dict) -> bool:
        anchor = v.get("anchor") or v.get("record")
        idx = v.get("index", 0)
        slot = v["slot"]
        matching = [b for b in blocks if b.anchor == anchor]
        if idx >= len(matching):
            return False
        block = matching[idx]
        for f in block.fields:
            if f.slot == slot:
                set_field(c, f, float(v["value"]))
                return True
        return False

    # ------------------------------------------------------------------ revert
    def revert(self) -> list[str]:
        """Restore every backed-up archive to vanilla."""
        restored = []
        if not self.backups_dir.exists():
            return restored
        for bak in self.backups_dir.rglob("*.smb"):
            rel = bak.relative_to(self.backups_dir)
            shutil.copy2(bak, self.game_data / rel)
            restored.append(str(rel).replace("\\", "/"))
        if self.installed_file.exists():
            self.installed_file.unlink()
        return restored
