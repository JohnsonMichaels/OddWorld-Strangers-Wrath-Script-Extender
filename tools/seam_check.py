"""Measure whether textures still tile seamlessly.

Terrain textures repeat across the landscape, so their edges must wrap: the
right-hand column has to continue naturally into the left-hand column. AI
upscalers process images in padded tiles and can alter edge pixels, which
breaks that wrap and produces a visible grid of seams across the whole ground -
the classic failure mode when upscaling tiling textures.

This measures it instead of guessing. For each image:

    wrap difference  = mean |left column  - right column |
    interior difference = mean |adjacent interior columns|
    ratio = wrap / interior

A seamless texture wraps about as smoothly as any two neighbouring columns, so
the ratio sits near 1.0. A broken wrap makes the edges disagree far more than
the interior does, pushing the ratio well above 1.

Run it on the ORIGINALS first to learn which textures were tiling to begin with
(many are not - decals, signs and one-off surfaces never tile), then on the
UPSCALED set and compare. What matters is not the absolute number but whether
a texture that tiled before still tiles after.

Usage:
  python tools/seam_check.py <dir> [--csv out.csv] [--limit N]
  python tools/seam_check.py <dir-a> --compare <dir-b>
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image


def edge_ratios(path: Path) -> tuple[float, float]:
    """Return (horizontal_ratio, vertical_ratio) for one image."""
    img = Image.open(path).convert("RGB")
    w, h = img.size
    px = img.load()

    def col_diff(x0: int, x1: int) -> float:
        t = 0
        for y in range(h):
            a, b = px[x0, y], px[x1, y]
            t += abs(a[0]-b[0]) + abs(a[1]-b[1]) + abs(a[2]-b[2])
        return t / (h * 3.0)

    def row_diff(y0: int, y1: int) -> float:
        t = 0
        for x in range(w):
            a, b = px[x, y0], px[x, y1]
            t += abs(a[0]-b[0]) + abs(a[1]-b[1]) + abs(a[2]-b[2])
        return t / (w * 3.0)

    # wrap: last column against first column
    wrap_h = col_diff(w - 1, 0)
    wrap_v = row_diff(h - 1, 0)
    # interior baseline: a few neighbouring pairs, averaged
    inner_h = sum(col_diff(x, x + 1) for x in (w//4, w//2, 3*w//4)) / 3.0
    inner_v = sum(row_diff(y, y + 1) for y in (h//4, h//2, 3*h//4)) / 3.0

    rh = wrap_h / inner_h if inner_h > 0.01 else 0.0
    rv = wrap_v / inner_v if inner_v > 0.01 else 0.0
    return rh, rv


def scan(d: Path, limit: int = 0) -> dict[str, tuple[float, float]]:
    out = {}
    files = sorted(d.glob("*.png"))
    if limit:
        files = files[:limit]
    for i, p in enumerate(files):
        try:
            out[p.name] = edge_ratios(p)
        except Exception as e:
            print(f"  !! {p.name}: {e}")
        if i and i % 25 == 0:
            print(f"  ...{i}/{len(files)}")
    return out


def verdict(rh: float, rv: float) -> str:
    r = max(rh, rv)
    if r <= 1.6:
        return "tiles"
    if r <= 3.0:
        return "weak"
    return "does not tile"


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    d = Path(args[0])
    compare = None
    limit = 0
    csv = None
    i = 1
    while i < len(args):
        if args[i] == "--compare" and i + 1 < len(args):
            compare = Path(args[i + 1]); i += 2
        elif args[i] == "--limit" and i + 1 < len(args):
            limit = int(args[i + 1]); i += 2
        elif args[i] == "--csv" and i + 1 < len(args):
            csv = Path(args[i + 1]); i += 2
        else:
            print(f"unknown argument: {args[i]}"); return 1

    print(f"scanning {d} ...")
    a = scan(d, limit)
    if not a:
        print("no PNGs found")
        return 1

    if compare is None:
        tiling = [n for n, (rh, rv) in a.items() if verdict(rh, rv) == "tiles"]
        weak = [n for n, (rh, rv) in a.items() if verdict(rh, rv) == "weak"]
        print(f"\n{len(a)} texture(s)")
        print(f"  tile seamlessly : {len(tiling)}")
        print(f"  borderline      : {len(weak)}")
        print(f"  do not tile     : {len(a) - len(tiling) - len(weak)}")
        print("\nworst 10 (highest edge mismatch):")
        for n, (rh, rv) in sorted(a.items(), key=lambda kv: -max(kv[1]))[:10]:
            print(f"  {n}  h={rh:5.2f} v={rv:5.2f}  {verdict(rh, rv)}")
        if csv:
            with open(csv, "w", encoding="utf-8") as f:
                f.write("file,h_ratio,v_ratio,verdict\n")
                for n, (rh, rv) in sorted(a.items()):
                    f.write(f"{n},{rh:.3f},{rv:.3f},{verdict(rh, rv)}\n")
            print(f"\nwrote {csv}")
        return 0

    print(f"scanning {compare} ...")
    b = scan(compare, limit)
    broken = []
    kept = 0
    for n, (rh, rv) in a.items():
        if n not in b:
            continue
        was, now = verdict(rh, rv), verdict(*b[n])
        if was == "tiles" and now != "tiles":
            broken.append((n, max(rh, rv), max(b[n])))
        elif was == "tiles":
            kept += 1
    print(f"\ncompared {len(b)} texture(s)")
    print(f"  tiled before and still tiles : {kept}")
    print(f"  TILING BROKEN by upscaling   : {len(broken)}")
    for n, before, after in sorted(broken, key=lambda t: -t[2])[:15]:
        print(f"    {n}  {before:.2f} -> {after:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
