"""List exactly which textures lost their tiling during upscaling.

seam_check --compare prints a truncated summary; packing decisions need the
full list, and the fix (tools/seam_fix.py) needs it as input so it only touches
textures that actually regressed.

A texture is "broken" if it tiled before (both ratios near 1) and no longer
does. Textures that never tiled are irrelevant - an upscaler cannot break a
wrap that was not there.

USAGE
  python tools/seam_diff.py <orig.csv> <upscaled.csv> [--out broken.txt]
                            [--tiled 1.6] [--broken 2.0]
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path


def load(p: Path) -> dict[str, tuple[float, float]]:
    out: dict[str, tuple[float, float]] = {}
    with p.open(newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            try:
                out[Path(row["file"]).stem.upper()] = (
                    float(row["h_ratio"]), float(row["v_ratio"]))
            except (KeyError, ValueError):
                continue
    return out


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    a = load(Path(sys.argv[1]))
    b = load(Path(sys.argv[2]))

    out_path = Path("broken_wrap.txt")
    tiled_thr, broken_thr = 1.6, 2.0
    args = sys.argv[3:]
    i = 0
    while i < len(args):
        if args[i] == "--out" and i + 1 < len(args):
            out_path = Path(args[i + 1]); i += 2
        elif args[i] == "--tiled" and i + 1 < len(args):
            tiled_thr = float(args[i + 1]); i += 2
        elif args[i] == "--broken" and i + 1 < len(args):
            broken_thr = float(args[i + 1]); i += 2
        else:
            i += 1

    broken, kept, missing = [], [], []
    for name, (h0, v0) in a.items():
        if max(h0, v0) > tiled_thr:
            continue                      # never tiled: cannot be broken
        if name not in b:
            missing.append(name)
            continue
        h1, v1 = b[name]
        if max(h1, v1) > broken_thr:
            broken.append((name, max(h0, v0), max(h1, v1)))
        else:
            kept.append(name)

    broken.sort(key=lambda r: -r[2])
    out_path.write_text("\n".join(n for n, _, _ in broken), encoding="utf-8")

    print(f"tiling textures in original : {len(broken) + len(kept) + len(missing)}")
    print(f"  still tile after upscaling : {len(kept)}")
    print(f"  BROKEN                     : {len(broken)}")
    if missing:
        print(f"  missing from upscaled set  : {len(missing)}")
    print(f"\nwrote {out_path} ({len(broken)} names)")
    for name, r0, r1 in broken[:10]:
        print(f"  {name}  {r0:.2f} -> {r1:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
