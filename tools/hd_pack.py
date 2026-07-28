"""Bulk-pack AI-upscaled PNGs into SWSE HD runtime replacements (.oft).

Step 3 of the HD texture pipeline:

    hd_export.py  ->  <dir>/*.png  ->  [ chaiNNer / ESRGAN, 2x ]  ->  hd_pack.py

Input is a folder of PNGs still named <FINGERPRINT>.png (upscalers preserve
filenames), so no lookup table is needed: the filename IS the key SWSE matches
at GPU-upload time.

The PNG becomes mip 0 at whatever resolution the upscaler produced. Smaller
mips are generated with plain Lanczos, which is correct -- mips should be clean
downsamples; the AI detail belongs at level 0 only.

Archives are never touched. The engine keeps loading vanilla data and SWSE
substitutes ours at upload, so a bad texture can never corrupt a save or crash
the level loader -- worst case it looks wrong and you delete the file.

Usage:
  python tools/hd_pack.py <upscaled-png-dir> [--out <dir>] [--dry-run]
                                             [--max-size 4096]

  --max-size  refuse textures above this dimension (default 4096).
              stranger.exe is 32-bit: 2x dimensions is 4x texture memory, and
              the address space is finite. Raise deliberately, not by accident.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PIL import Image

from oddforge.dxt import encode_dxt1, mip_sizes

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath")
GL_DXT1_RGBA = 0x83F1     # what the engine uses for fmt-12 textures


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    src = Path(args[0])
    out_dir = GAME / "SWSEMods" / "SWSE HD" / "textures"
    dry = False
    max_size = 4096
    i = 1
    while i < len(args):
        if args[i] == "--out" and i + 1 < len(args):
            out_dir = Path(args[i + 1]); i += 2
        elif args[i] == "--dry-run":
            dry = True; i += 1
        elif args[i] == "--max-size" and i + 1 < len(args):
            max_size = int(args[i + 1]); i += 2
        else:
            print(f"unknown argument: {args[i]}")
            return 1

    pngs = sorted(src.glob("*.png"))
    if not pngs:
        print(f"no PNGs in {src}")
        return 1
    print(f"{len(pngs)} PNG(s) in {src}")
    if not dry:
        out_dir.mkdir(parents=True, exist_ok=True)

    n_ok = n_bad_name = n_bad_dim = n_too_big = 0
    total_bytes = 0
    for p in pngs:
        stem = p.stem
        # The filename must still be the fingerprint the exporter assigned.
        if len(stem) != 8 or any(ch not in "0123456789abcdefABCDEF" for ch in stem):
            n_bad_name += 1
            continue

        img = Image.open(p).convert("RGB")
        w, h = img.size
        # DXT1 needs power-of-two here because the engine's mip rules assume it.
        if w & (w - 1) or h & (h - 1):
            print(f"  skip {p.name}: {w}x{h} is not power-of-two")
            n_bad_dim += 1
            continue
        if max(w, h) > max_size:
            print(f"  skip {p.name}: {w}x{h} exceeds --max-size {max_size}")
            n_too_big += 1
            continue

        # mip chain down to the engine's floor (min dim 8)
        levels = []
        for mw, mh, _ in mip_sizes(w, h, min_dim=8):
            lvl = img if (mw, mh) == img.size else img.resize((mw, mh), Image.LANCZOS)
            levels.append(encode_dxt1(lvl))

        blob = bytearray(b"OFT1")
        blob += struct.pack("<4I", GL_DXT1_RGBA, w, h, len(levels))
        for lv in levels:
            blob += struct.pack("<I", len(lv))
            blob += lv

        total_bytes += len(blob)
        n_ok += 1
        if not dry:
            (out_dir / f"{stem.upper()}.oft").write_bytes(bytes(blob))

    print(f"\n{'would write' if dry else 'wrote'} {n_ok} .oft file(s)"
          f" -> {out_dir}")
    print(f"  total size        : {total_bytes / 1048576:.1f} MB")
    if n_bad_name:
        print(f"  skipped (filename not a fingerprint): {n_bad_name}")
    if n_bad_dim:
        print(f"  skipped (not power-of-two)          : {n_bad_dim}")
    if n_too_big:
        print(f"  skipped (over --max-size)           : {n_too_big}")
    if not dry:
        print("\nRestart the game; SWSE swaps them in at texture-upload time.")
        print("To undo, delete the .oft files - archives were never modified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
