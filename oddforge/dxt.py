"""DXT1 (BC1) texture codec + mip chain layout for Stranger's Wrath SMB textures.

Layout (verified against global_loadingscreen_*.smb):
- top mip at texture data offset 0, subsequent mips packed consecutively
- mip chain runs down to 8x8 (e.g. 1024: 1024..8 = 8 levels, 699,040 data bytes;
  observed per-texture stride 699,136 leaves a 96B tail gap we preserve untouched)
"""
from __future__ import annotations

import struct

from PIL import Image


def mip_sizes(width: int, height: int, min_dim: int = 8) -> list[tuple[int, int, int]]:
    """[(w, h, byte_size), ...] for a DXT1 mip chain down to min_dim."""
    out = []
    w, h = width, height
    while w >= min_dim and h >= min_dim:
        out.append((w, h, max(w // 4, 1) * max(h // 4, 1) * 8))
        w //= 2
        h //= 2
    return out


def decode_dxt1(data: bytes, width: int, height: int) -> Image.Image:
    img = Image.new("RGB", (width, height))
    px = img.load()
    off = 0
    for by in range(height // 4):
        for bx in range(width // 4):
            c0, c1, bits = struct.unpack_from("<HHI", data, off)
            off += 8

            def expand(c: int) -> tuple[int, int, int]:
                return (((c >> 11) & 0x1F) * 255 // 31,
                        ((c >> 5) & 0x3F) * 255 // 63,
                        (c & 0x1F) * 255 // 31)

            p0, p1 = expand(c0), expand(c1)
            if c0 > c1:
                pal = [p0, p1,
                       tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
            else:
                pal = [p0, p1,
                       tuple((a + b) // 2 for a, b in zip(p0, p1)),
                       (0, 0, 0)]
            for py in range(4):
                for pxi in range(4):
                    idx = (bits >> (2 * (py * 4 + pxi))) & 3
                    px[bx * 4 + pxi, by * 4 + py] = pal[idx]
    return img


def decode_dxt5(data: bytes, width: int, height: int) -> Image.Image:
    """DXT5 (BC3): 8B interpolated alpha block + 8B DXT1-style color block per 4x4."""
    img = Image.new("RGBA", (width, height))
    px = img.load()
    off = 0
    for by in range(height // 4):
        for bx in range(width // 4):
            a0, a1 = data[off], data[off + 1]
            abits = int.from_bytes(data[off + 2 : off + 8], "little")
            c0, c1, bits = struct.unpack_from("<HHI", data, off + 8)
            off += 16
            if a0 > a1:
                apal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
            else:
                apal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]

            def expand(c: int) -> tuple[int, int, int]:
                return (((c >> 11) & 0x1F) * 255 // 31,
                        ((c >> 5) & 0x3F) * 255 // 63,
                        (c & 0x1F) * 255 // 31)

            p0, p1 = expand(c0), expand(c1)
            pal = [p0, p1,
                   tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
                   tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
            for py in range(4):
                for pxi in range(4):
                    n = py * 4 + pxi
                    ai = (abits >> (3 * n)) & 7
                    ci = (bits >> (2 * n)) & 3
                    px[bx * 4 + pxi, by * 4 + py] = (*pal[ci], apal[ai])
    return img


def _to565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def encode_dxt1(img: Image.Image) -> bytes:
    """Simple DXT1 encoder (min/max endpoints by luminance, opaque 4-color mode)."""
    img = img.convert("RGB")
    w, h = img.size
    px = img.load()
    out = bytearray()
    for by in range(h // 4):
        for bx in range(w // 4):
            block = [px[bx * 4 + i, by * 4 + j] for j in range(4) for i in range(4)]
            lum = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in block]
            lo = block[lum.index(min(lum))]
            hi = block[lum.index(max(lum))]
            c0, c1 = _to565(*hi), _to565(*lo)
            if c0 == c1:
                out += struct.pack("<HHI", c0, c1 if c0 == 0 else 0, 0)
                continue
            if c0 < c1:
                c0, c1 = c1, c0
                hi, lo = lo, hi
            pal = [hi, lo,
                   tuple((2 * a + b) // 3 for a, b in zip(hi, lo)),
                   tuple((a + 2 * b) // 3 for a, b in zip(hi, lo))]
            bits = 0
            for n, (r, g, b) in enumerate(block):
                best = min(range(4), key=lambda k: (r - pal[k][0]) ** 2
                           + (g - pal[k][1]) ** 2 + (b - pal[k][2]) ** 2)
                bits |= best << (2 * n)
            out += struct.pack("<HHI", c0, c1, bits)
    return bytes(out)


def encode_mip_chain(img: Image.Image, min_dim: int = 8) -> bytes:
    """Encode an image and its full mip chain as consecutive DXT1 data."""
    out = bytearray()
    for w, h, _ in mip_sizes(*img.size, min_dim=min_dim):
        level = img if (w, h) == img.size else img.resize((w, h), Image.LANCZOS)
        out += encode_dxt1(level)
    return bytes(out)
