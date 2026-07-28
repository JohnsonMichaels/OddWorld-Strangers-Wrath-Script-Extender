"""Halve AI-upscaled textures from 4x to 2x.

WHY THIS STEP EXISTS. Ultrasharp is a native 4x model, so the only way to get
a 2x result from it is to upscale 4x and then downsample - which is exactly
what Upscayl's GUI does when set to 2x. Running the model at 4x and keeping
that output would quadruple texture memory instead of doubling it, and
stranger.exe is a 32-bit process with a finite address space.

Lanczos for the downsample: the AI detail belongs at the final resolution, and
a good filter preserves it without the ringing a naive box filter leaves on
hard edges.

Filenames are preserved, because the whole pipeline keys on the texture's
runtime FINGERPRINT being the filename.

USAGE
  python tools/downscale.py <in-dir> <out-dir> [--factor 2]
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image


def main() -> int:
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1
    src, dst = Path(args[0]), Path(args[1])
    factor = 2
    if "--factor" in args:
        factor = int(args[args.index("--factor") + 1])
    dst.mkdir(parents=True, exist_ok=True)

    files = sorted(p for p in src.iterdir() if p.suffix.lower() == ".png")
    done = 0
    for p in files:
        im = Image.open(p)
        w, h = im.width // factor, im.height // factor
        if w < 1 or h < 1:
            continue
        im.resize((w, h), Image.LANCZOS).save(dst / p.name)
        done += 1
        if done % 100 == 0:
            print(f"  ...{done}/{len(files)}", flush=True)
    print(f"downscaled {done} texture(s) by {factor}x -> {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
