"""Bulk-export game textures as PNGs, ready for AI upscaling.

Step 1 of the HD texture pipeline:

    hd_export.py  ->  <out>/*.png   ->  [ chaiNNer / ESRGAN, 2x ]  ->  hd_pack.py

Each PNG is named after the texture's RUNTIME FINGERPRINT, not its archive
name. That is the whole trick: the fingerprint is what SWSE matches at GPU
upload time, so if the upscaler preserves filenames (chaiNNer and Cupscale
both do) the packing step needs no lookup table and no manual matching at all.
Duplicate textures across archives collapse onto one file for free, which
matters here -- there are 1224 archives and plenty of shared art.

Only DXT1 textures are handled, because that is what the encoder supports.
Others are counted and skipped rather than silently mangled.

Usage:
  python tools/hd_export.py <out-dir> [--filter frag[,frag...]] [--limit N]
                                      [--min-size 64]

  --filter    only textures whose archive path or name contains a fragment.
              Use this to pilot a small set first, e.g.
                  --filter crossbow,ammo
  --min-size  skip textures smaller than this (default 64). Tiny textures are
              usually UI//effects where AI upscaling does more harm than good.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from oddforge.container import SmbContainer
from oddforge.textures import decode_entry
from oddforge.toc import find_textures

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath")

FNV_OFFSET, FNV_PRIME = 2166136261, 16777619


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


def fingerprint(mip0: bytes, w: int, h: int) -> int:
    """Must stay byte-for-byte identical to Fnv1a/fingerprint in glspy.cpp."""
    return (fnv1a(mip0[:4096])
            ^ ((w * 73856093) & 0xFFFFFFFF)
            ^ ((h * 19349663) & 0xFFFFFFFF)) & 0xFFFFFFFF


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    out_dir = Path(args[0])
    frags: list[str] = []
    limit = 0
    min_size = 64
    i = 1
    while i < len(args):
        if args[i] == "--filter" and i + 1 < len(args):
            frags = [f.strip().lower() for f in args[i + 1].split(",") if f.strip()]
            i += 2
        elif args[i] == "--limit" and i + 1 < len(args):
            limit = int(args[i + 1]); i += 2
        elif args[i] == "--min-size" and i + 1 < len(args):
            min_size = int(args[i + 1]); i += 2
        else:
            print(f"unknown argument: {args[i]}")
            return 1

    out_dir.mkdir(parents=True, exist_ok=True)
    archives = sorted(GAME.glob("data/**/*.smb"))
    print(f"{len(archives)} archives under {GAME}")
    if frags:
        print(f"filter: {frags}")

    seen: dict[int, str] = {}
    manifest = []
    n_written = n_skipped_fmt = n_skipped_small = n_dupe = 0

    for ai, arc in enumerate(archives):
        rel = str(arc.relative_to(GAME)).replace("\\", "/")
        try:
            c = SmbContainer.parse_file(arc)
            texs = find_textures(c)
        except Exception as e:                       # a bad archive must not stop the run
            print(f"  !! {rel}: {e}")
            continue

        for t in texs:
            hay = f"{rel}/{t.name}".lower()
            if frags and not any(f in hay for f in frags):
                continue
            if not t.is_dxt1:
                n_skipped_fmt += 1
                continue
            if min(t.width, t.height) < min_size:
                n_skipped_small += 1
                continue

            mip0_len = (t.width // 4) * (t.height // 4) * 8
            mip0 = c.section1[t.data_offset: t.data_offset + mip0_len]
            if len(mip0) < mip0_len:
                continue
            fp = fingerprint(mip0, t.width, t.height)
            if fp in seen:
                n_dupe += 1
                continue

            try:
                img = decode_entry(c, t)
            except Exception as e:
                print(f"  !! decode {t.name}: {e}")
                continue
            if img is None:
                continue

            png = out_dir / f"{fp:08X}.png"
            img.save(png)
            seen[fp] = t.name
            manifest.append((f"{fp:08X}", t.name, t.width, t.height, rel))
            n_written += 1
            if limit and n_written >= limit:
                break

        if limit and n_written >= limit:
            break
        if ai % 100 == 0 and ai:
            print(f"  ...{ai}/{len(archives)} archives, {n_written} textures")

    man = out_dir / "manifest.tsv"
    with open(man, "w", encoding="utf-8") as f:
        f.write("fingerprint\tname\twidth\theight\tarchive\n")
        for row in manifest:
            f.write("\t".join(str(x) for x in row) + "\n")

    print(f"\nwrote {n_written} PNG(s) to {out_dir}")
    print(f"  duplicates collapsed : {n_dupe}")
    print(f"  skipped (not DXT1)   : {n_skipped_fmt}")
    print(f"  skipped (< {min_size}px)    : {n_skipped_small}")
    print(f"  manifest             : {man}")
    print("\nNext: upscale the PNGs 2x with an AI model (chaiNNer / ESRGAN),")
    print("KEEPING THE FILENAMES, then run hd_pack.py on the output folder.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
