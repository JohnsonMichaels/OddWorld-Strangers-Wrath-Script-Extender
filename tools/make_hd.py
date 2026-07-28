"""Build an SWSE HD runtime-replacement texture (.oft).

Fingerprints the texture as it currently sits in the archive (that's the data
the engine uploads - the fingerprint SWSE sees at runtime), encodes your
replacement image as a DXT1 mip chain AT ITS OWN RESOLUTION, and writes
  <game>\\SWSEMods\\SWSE HD\\textures\\<HASH>.oft
SWSE swaps it in at GPU-upload time. Archives stay vanilla-structured;
any resolution works because the engine never parses the replacement.

Usage:
  python tools\\make_hd.py <archive.smb> <texture-name-fragment> <image.png>
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PIL import Image

from oddforge.container import SmbContainer
from oddforge.dxt import encode_dxt1, mip_sizes
from oddforge.textures import dxt1_chain_nbytes
from oddforge.toc import find_textures

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath")
GL_DXT1_RGBA = 0x83F1     # what the engine uses for fmt-12 textures

FNV_OFFSET, FNV_PRIME = 2166136261, 16777619


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


def fingerprint(mip0: bytes, w: int, h: int) -> int:
    return (fnv1a(mip0[:4096])
            ^ ((w * 73856093) & 0xFFFFFFFF)
            ^ ((h * 19349663) & 0xFFFFFFFF)) & 0xFFFFFFFF


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    archive, frag, image = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])

    c = SmbContainer.parse_file(archive)
    t = next((x for x in find_textures(c) if frag.lower() in x.name.lower()
              and x.is_dxt1), None)
    if t is None:
        print(f"no DXT1 texture matching '{frag}' in {archive.name}")
        return 1

    mip0_len = (t.width // 4) * (t.height // 4) * 8
    mip0 = c.section1[t.data_offset : t.data_offset + mip0_len]
    fp = fingerprint(mip0, t.width, t.height)
    print(f"target: {t.name} ({t.width}x{t.height}) fingerprint={fp:08X}")

    img = Image.open(image).convert("RGB")
    w, h = img.size
    if w & (w - 1) or h & (h - 1):
        print(f"image must be power-of-two, got {w}x{h}")
        return 1

    # chain to the engine's floor (min dim 8)
    levels = []
    for mw, mh, _ in mip_sizes(w, h, min_dim=8):
        lvl = img if (mw, mh) == img.size else img.resize((mw, mh), Image.LANCZOS)
        levels.append(encode_dxt1(lvl))
    print(f"encoded {len(levels)} levels at {w}x{h}")

    out_dir = GAME / "SWSEMods" / "SWSE HD" / "textures"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"{fp:08X}.oft"
    with open(out, "wb") as f:
        f.write(b"OFT1")
        f.write(struct.pack("<4I", GL_DXT1_RGBA, w, h, len(levels)))
        for lv in levels:
            f.write(struct.pack("<I", len(lv)))
            f.write(lv)
    print(f"written: {out}")
    print("SWSE will swap it in at texture-upload time. Restart the game.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
