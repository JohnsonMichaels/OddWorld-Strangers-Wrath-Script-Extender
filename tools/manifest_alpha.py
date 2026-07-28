"""Check an EXPORTED SET for DXT1 1-bit alpha before it is upscaled and packed.

Why this matters: our pipeline drops alpha in both directions (decode_entry
returns RGB, encode_dxt1 calls convert("RGB")). A texture that relies on DXT1's
1-bit alpha therefore comes back fully opaque, and every formerly-transparent
pixel renders as solid black. On foliage that means leaf cards become black
rectangles - far more visible than on the character set, where the same check
found zero affected textures.

Detection is exact, not a filename guess: a DXT1 block encodes two 16-bit
colours, and if color0 <= color1 the block switches to 3-colour mode where
index 3 means TRANSPARENT.

Usage:
  python tools/manifest_alpha.py hd_textures/env
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from oddforge.container import SmbContainer
from oddforge.toc import find_textures

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath")


def uses_alpha(dxt1: bytes) -> bool:
    for off in range(0, len(dxt1) - 7, 8):
        c0, c1 = struct.unpack_from("<HH", dxt1, off)
        if c0 > c1:
            continue
        bits = struct.unpack_from("<I", dxt1, off + 4)[0]
        for i in range(16):
            if (bits >> (i * 2)) & 3 == 3:
                return True
    return False


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    d = Path(sys.argv[1])
    man = d / "manifest.tsv"
    if not man.exists():
        print(f"no manifest at {man}")
        return 1

    want: dict[str, tuple[str, str]] = {}
    for line in man.read_text(encoding="utf-8").splitlines()[1:]:
        p = line.split("\t")
        if len(p) >= 5:
            want[p[0].upper()] = (p[1], p[4])
    print(f"{len(want)} textures in {d.name}")

    by_arc: dict[str, set[str]] = {}
    for fp, (name, arc) in want.items():
        by_arc.setdefault(arc, set()).add(fp)

    offenders = []
    done = 0
    for arc, fps in sorted(by_arc.items()):
        path = GAME / arc
        if not path.exists():
            continue
        try:
            c = SmbContainer.parse_file(path)
            texs = find_textures(c)
        except Exception:
            continue
        for t in texs:
            if not t.is_dxt1:
                continue
            n = (t.width // 4) * (t.height // 4) * 8
            mip0 = c.section1[t.data_offset: t.data_offset + n]
            if len(mip0) < n:
                continue
            from tools.hd_export import fingerprint  # reuse the exact hash
            fp = f"{fingerprint(mip0, t.width, t.height):08X}"
            if fp not in fps:
                continue
            done += 1
            if uses_alpha(mip0):
                offenders.append((fp, t.name, t.width, t.height))

    print(f"checked {done}")
    print(f"\n{len(offenders)} texture(s) NEED 1-bit alpha - packing these would")
    print("render their transparent areas as SOLID BLACK:")
    for fp, name, w, h in sorted(offenders, key=lambda x: x[1]):
        print(f"  {fp}  {name}  ({w}x{h})")
    if not offenders:
        print("  (none - safe to upscale and pack)")
    else:
        q = d / "needs_alpha"
        q.mkdir(exist_ok=True)
        moved = 0
        for fp, *_ in offenders:
            src = d / f"{fp}.png"
            if src.exists():
                src.replace(q / src.name)
                moved += 1
        print(f"\nmoved {moved} PNG(s) to {q.name}/ so they are not upscaled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
