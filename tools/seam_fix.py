"""Restore seamless tiling to upscaled textures.

THE PROBLEM
    AI upscalers process an image in padded tiles and alter the outermost
    pixels. For a texture that repeats across terrain, that breaks the wrap:
    the right-hand column no longer continues into the left-hand column, and
    the ground shows a grid of seams. Measured on the environment batch, 63 of
    71 tiling textures lost their wrap during upscaling.

THE FIX
    The interior detail is fine - only the edges moved. So blend each edge
    toward its wrap partner, with the correction fading out over a few pixels:

        new_left[x]  = mix(left[x],  right_partner[x], w(x))
        new_right[x] = mix(right[x], left_partner[x],  w(x))

    with w = 0.5 at the very edge falling to 0 by `feather` pixels in. Both
    sides move halfway toward each other at the boundary, so they meet exactly
    and the seam disappears, while pixels a few steps in are barely touched.

    A feather of 4-8 pixels on a 512-1024px texture is a fraction of a percent
    of the image and invisible in play, whereas a broken wrap is visible from
    across the level.

USAGE
  python tools/seam_fix.py <dir> [--out <dir>] [--feather 6] [--only list.txt]

  Without --out the files are rewritten in place. --only takes a file of
  names (one per line) so a fix can be limited to the textures seam_check
  reported as broken, leaving everything else untouched.
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image


def blend_edges(im: Image.Image, feather: int) -> Image.Image:
    im = im.convert("RGBA") if im.mode == "RGBA" else im.convert("RGB")
    px = im.load()
    w, h = im.size
    n = min(feather, w // 4, h // 4)
    if n < 1:
        return im

    bands = len(im.getbands())

    # Horizontal wrap: left edge <-> right edge.
    for x in range(n):
        # 0.5 at the boundary, fading to 0 by `n` pixels in.
        t = 0.5 * (1.0 - x / n)
        xl, xr = x, w - 1 - x
        for y in range(h):
            a = px[xl, y]
            b = px[xr, y]
            na = tuple(int(round(a[c] + (b[c] - a[c]) * t)) for c in range(bands))
            nb = tuple(int(round(b[c] + (a[c] - b[c]) * t)) for c in range(bands))
            px[xl, y] = na
            px[xr, y] = nb

    # Vertical wrap: top edge <-> bottom edge.
    for y in range(n):
        t = 0.5 * (1.0 - y / n)
        yt, yb = y, h - 1 - y
        for x in range(w):
            a = px[x, yt]
            b = px[x, yb]
            na = tuple(int(round(a[c] + (b[c] - a[c]) * t)) for c in range(bands))
            nb = tuple(int(round(b[c] + (a[c] - b[c]) * t)) for c in range(bands))
            px[x, yt] = na
            px[x, yb] = nb

    return im


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1

    src = Path(args[0])
    out = src
    feather = 6
    only: set[str] | None = None

    i = 1
    while i < len(args):
        if args[i] == "--out" and i + 1 < len(args):
            out = Path(args[i + 1]); i += 2
        elif args[i] == "--feather" and i + 1 < len(args):
            feather = int(args[i + 1]); i += 2
        elif args[i] == "--only" and i + 1 < len(args):
            names = Path(args[i + 1]).read_text(encoding="utf-8").split()
            only = {Path(n).stem.upper() for n in names if n.strip()}
            i += 2
        else:
            i += 1

    if not src.is_dir():
        print(f"not a directory: {src}")
        return 1
    out.mkdir(parents=True, exist_ok=True)

    files = sorted(p for p in src.iterdir() if p.suffix.lower() == ".png")
    done = skipped = 0
    for p in files:
        if only is not None and p.stem.upper() not in only:
            skipped += 1
            continue
        im = Image.open(p)
        im = blend_edges(im, feather)
        im.save(out / p.name)
        done += 1
        if done % 25 == 0:
            print(f"  ...{done}")

    print(f"\nfixed {done} texture(s), skipped {skipped}, feather {feather}px")
    print(f"out: {out}")
    print("re-run seam_check --compare against the ORIGINALS to confirm.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
