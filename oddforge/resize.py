"""EXPERIMENTAL: true texture up-resolution (grow the archive).

Unlike textures.replace_entry (same-size, in-place), this relays section1:
the texture's DXT1 mip chain is re-encoded at the new dimensions, spliced in,
and every structure that references section1 is fixed up:

  * data_offset in every definition-node header AFTER the grown blob
  * width/height in the texture's own TOC record (engine computes data size
    from dims+mips+fmt - no stored size field, verified across 413 textures)
  * container header: sec1_size (C) and sec1_used (E)

The insert is padded to 128 bytes: EVERY vanilla blob offset is 128-aligned
(617/617 sampled), so the shift must be a multiple of 128 to keep every
downstream blob on the engine's alignment. (16-byte padding crashed the game.)
"""
from __future__ import annotations

import struct

from PIL import Image

from .container import SmbContainer
from .dxt import encode_dxt1, mip_sizes
from .textures import dxt1_chain_nbytes
from .toc import TextureEntry, _iter_names, _read_node_header

ALIGN = 128


def _encode_chain(img: Image.Image, w: int, h: int, mips: int) -> bytes:
    img = img.convert("RGB")
    if img.size != (w, h):
        img = img.resize((w, h), Image.LANCZOS)
    out = bytearray()
    for mw, mh, _ in mip_sizes(w, h, min_dim=1)[:mips]:
        level = img if (mw, mh) == img.size else img.resize((mw, mh), Image.LANCZOS)
        out += encode_dxt1(level)
    return bytes(out)


def resize_entry(c: SmbContainer, t: TextureEntry, new_img: Image.Image,
                 new_w: int, new_h: int) -> int:
    """Grow texture t to new_w x new_h in-place in container c.

    Returns the number of bytes section1 grew by. Raises on non-DXT1 or
    non-power-of-two target dims.
    """
    if not t.is_dxt1:
        raise ValueError(f"{t.name}: only DXT1 upres supported")
    if new_w & (new_w - 1) or new_h & (new_h - 1):
        raise ValueError(f"target dims must be power-of-two, got {new_w}x{new_h}")

    # ENGINE RULE (526/526 vanilla textures): the mip chain must end at
    # min-dim 8, i.e. mips = log2(min(w,h)) - 2. The engine derives data
    # size from dims assuming this; wrong mip count = crash.
    new_mips = max(1, min(new_w, new_h).bit_length() - 1 - 2)

    old_len = dxt1_chain_nbytes(t.width, t.height, t.mips)
    chain = _encode_chain(new_img, new_w, new_h, new_mips)
    # pad the insert so every later blob keeps its current alignment
    grow_raw = len(chain) - old_len
    pad = (-grow_raw) % ALIGN
    grow = grow_raw + pad

    # --- splice section1 ---
    sec1 = bytearray(c.section1)
    sec1[t.data_offset : t.data_offset + old_len] = chain + b"\x00" * pad
    c.section1 = bytes(sec1)

    # --- fix the TOC: shift later node offsets, patch our record's dims ---
    toc = bytearray(c.toc)
    patched_dims = False
    for name_off, name, end in _iter_names(bytes(toc), 4):
        hdr = _read_node_header(bytes(toc), name_off)
        if hdr is None:
            continue
        hpos = name_off - 40
        if hdr[3] > t.data_offset:                      # nodes after the grown blob
            struct.pack_into("<I", toc, hpos + 12, hdr[3] + grow)
        elif hdr[3] == t.data_offset and name == t.name:
            # our texture record: patch width/height/mips (fmt unchanged)
            struct.pack_into("<I", toc, end + 25, new_w)
            struct.pack_into("<I", toc, end + 29, new_h)
            struct.pack_into("<I", toc, end + 33, new_mips)
            patched_dims = True
    if not patched_dims:
        raise ValueError(f"{t.name}: TOC record not found for dim patch")
    c.toc = bytes(toc)

    # --- container header bookkeeping ---
    c.sec1_size += grow    # C
    c.sec1_used += grow    # E
    return grow
