"""Texture export (dump) - decode game textures to PNG for editing / AI upscaling.

The dump layout is EXACTLY what the mod loader consumes:

    <out>/art/textures/rocks/wall01.png   (from \\art\\textures\\rocks\\wall01.tga)

So the round-trip workflow is:
  1. Export:   python -m oddforge.dump "<game>\\data" "<dump folder>"
  2. Upscale:  run the PNGs through your AI upscaler (any output size is fine -
               reimport auto-fits them back to the game's native dimensions)
  3. Reimport: copy the folder into SWSEMods\\<YourMod>\\textures\\ and
               press APPLY ALL MODS in Studio (or ModLoader.apply()).

Only DXT1 (fmt 12) entries are decodable today; other formats are counted and
skipped. Duplicate texture paths across archives are exported once (first
archive wins - the same rule the mod loader's texture index uses).
"""
from __future__ import annotations

from pathlib import Path
from typing import Callable

from .container import SmbContainer
from .textures import decode_entry
from .toc import find_textures


def tex_relpath(name: str) -> str:
    """In-game texture path -> dump-relative PNG path (loader-compatible key)."""
    rel = name.lstrip("\\").replace("\\", "/")
    return rel.rsplit(".", 1)[0] + ".png"


def export_archive(smb_path: str | Path, out_dir: str | Path,
                   skip_existing: bool = False) -> tuple[int, int]:
    """Dump all decodable textures from one archive. Returns (exported, skipped).

    skipped counts entries we can't decode yet (non-DXT1) or that already
    exist in out_dir when skip_existing is set.
    """
    out_dir = Path(out_dir)
    c = SmbContainer.parse_file(smb_path)
    exported = skipped = 0
    for t in find_textures(c):
        dest = out_dir / tex_relpath(t.name)
        if skip_existing and dest.exists():
            skipped += 1
            continue
        img = decode_entry(c, t)
        if img is None:          # non-DXT1: not decodable yet
            skipped += 1
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        img.save(dest)
        exported += 1
    return exported, skipped


def export_all(game_data: str | Path, out_dir: str | Path,
               progress: Callable[[str, int, int], None] | None = None) -> dict:
    """Dump every texture from every .smb under game_data into out_dir.

    Returns {"archives": n, "exported": n, "skipped": n, "errors": n}.
    progress(archive_rel, done, total) is called per archive if given.
    """
    game_data = Path(game_data)
    smbs = sorted(game_data.rglob("*.smb"))   # sorted => same first-wins as loader
    stats = {"archives": 0, "exported": 0, "skipped": 0, "errors": 0}
    for i, smb in enumerate(smbs):
        rel = str(smb.relative_to(game_data))
        try:
            e, s = export_archive(smb, out_dir, skip_existing=True)
        except Exception:  # noqa: BLE001 - a bad archive shouldn't kill the dump
            stats["errors"] += 1
            continue
        stats["archives"] += 1
        stats["exported"] += e
        stats["skipped"] += s
        if progress:
            progress(rel, i + 1, len(smbs))
    return stats


def main(argv: list[str] | None = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(
        prog="python -m oddforge.dump",
        description="Dump game textures to PNG (mod-loader-compatible layout).")
    ap.add_argument("source", help=".smb archive OR game data folder to dump from")
    ap.add_argument("out", help="output folder for the PNG dump")
    args = ap.parse_args(argv)

    src, out = Path(args.source), Path(args.out)
    if src.is_file():
        e, s = export_archive(src, out)
        print(f"{src.name}: exported {e}, skipped {s} (non-DXT1)")
    else:
        def prog(rel: str, done: int, total: int) -> None:
            print(f"[{done}/{total}] {rel}", flush=True)
        st = export_all(src, out, progress=prog)
        print(f"\ndone: {st['exported']} textures exported from "
              f"{st['archives']} archives ({st['skipped']} skipped, "
              f"{st['errors']} unreadable archives)")
        print(f"dump folder: {out.resolve()}")
        print("next: AI-upscale the PNGs, then copy the folder into "
              "SWSEMods\\<YourMod>\\textures\\ and APPLY ALL MODS.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
