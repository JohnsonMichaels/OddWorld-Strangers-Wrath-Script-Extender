"""TOC (table of contents) parsing - texture entry enumeration.

Each TOC entry is preceded by a 40-byte node header:

    u32  timestamp   (0x4DFAA77E build time)
    u32  count?      (usually 1)
    u32  node_id     (hash)
    u32  data_offset (into section1)
    u32  unk4
    u32  unk5        (small; varies)
    u32  zero x3
    u32  node_id     (repeated - validation anchor)

Texture entries: after the name string, the record contains at fixed offsets:
    rec[12] == 0x29 (marker), fmt = u32@21, width = u32@25, height = u32@29,
    mips = u32@33.   fmt: 12 = DXT1 (verified), 15/18 = TBD (DXT5/uncompressed?)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .container import SmbContainer

TIMESTAMP = 0x4DFAA77E
TEXTURE_EXTS = (".tga", ".bmp", ".dds", ".png")

FMT_DXT1 = 12


@dataclass
class TextureEntry:
    name: str
    node_id: int
    data_offset: int      # into section1
    fmt: int
    width: int
    height: int
    mips: int

    @property
    def is_dxt1(self) -> bool:
        return self.fmt == FMT_DXT1


def _is_printable(b: bytes) -> bool:
    return all(32 <= c <= 126 for c in b)


def _iter_names(toc: bytes, start: int):
    """Yield (len_field_offset, name, end_offset) for path-like strings in the TOC."""
    i = start
    n = len(toc)
    while i + 4 <= n:
        (length,) = struct.unpack_from("<I", toc, i)
        if 6 <= length <= 260 and i + 4 + length <= n:
            chunk = toc[i + 4 : i + 4 + length]
            if _is_printable(chunk) and chunk[:1] == b"\\":
                yield i, chunk.decode("ascii"), i + 4 + length
                i += 4 + length
                continue
        i += 1


def _read_node_header(toc: bytes, name_off: int):
    """Validate + read the 40-byte node header preceding a name. None if invalid."""
    h = name_off - 40
    if h < 0:
        return None
    vals = struct.unpack_from("<10I", toc, h)
    if vals[0] != TIMESTAMP or vals[2] != vals[9]:
        return None
    return vals


def find_textures(c: SmbContainer) -> list[TextureEntry]:
    """Enumerate texture entries in an archive's TOC."""
    toc = c.toc
    out = []
    for name_off, name, end in _iter_names(toc, 4):
        if not name.lower().endswith(TEXTURE_EXTS):
            continue
        hdr = _read_node_header(toc, name_off)
        if hdr is None:
            continue  # reference node, not a definition
        # texture record follows the name
        if end + 37 > len(toc) or toc[end + 12] != 0x29:
            continue
        fmt, = struct.unpack_from("<I", toc, end + 21)
        w, = struct.unpack_from("<I", toc, end + 25)
        h, = struct.unpack_from("<I", toc, end + 29)
        mips, = struct.unpack_from("<I", toc, end + 33)
        if not (1 <= w <= 8192 and 1 <= h <= 8192 and 0 < mips <= 13 and fmt < 64):
            continue
        out.append(TextureEntry(name=name, node_id=hdr[2], data_offset=hdr[3],
                                fmt=fmt, width=w, height=h, mips=mips))
    return out
