"""High-level texture read/replace on top of container + toc + dxt."""
from __future__ import annotations

from pathlib import Path

from PIL import Image

from .container import SmbContainer
from .dxt import decode_dxt1, encode_dxt1, mip_sizes
from .toc import TextureEntry, find_textures


def dxt1_chain_nbytes(width: int, height: int, mips: int) -> int:
    sizes = mip_sizes(width, height, min_dim=1)[:mips]
    return sum(s for _, _, s in sizes)


def decode_entry(c: SmbContainer, t: TextureEntry) -> Image.Image | None:
    """Decode a texture entry's top mip to an RGB(A) image. None if unsupported."""
    if not t.is_dxt1:
        return None
    data = c.section1[t.data_offset : t.data_offset + (t.width // 4) * (t.height // 4) * 8]
    if len(data) < (t.width // 4) * (t.height // 4) * 8:
        return None
    return decode_dxt1(data, t.width, t.height)


def replace_entry(c: SmbContainer, t: TextureEntry, new_img: Image.Image) -> None:
    """Re-encode new_img (resized to entry dims) as a DXT1 mip chain, patch in place.

    Only same-dimension, DXT1 replacement is supported for now (keeps the archive
    byte-length identical, which the game accepts without complaint).
    """
    if not t.is_dxt1:
        raise ValueError(f"{t.name}: only DXT1 (fmt 12) replacement supported")
    img = new_img.convert("RGB")
    if img.size != (t.width, t.height):
        img = img.resize((t.width, t.height), Image.LANCZOS)

    # build the mip chain to match the original mip count
    out = bytearray()
    for w, h, _ in mip_sizes(t.width, t.height, min_dim=1)[: t.mips]:
        level = img if (w, h) == img.size else img.resize((w, h), Image.LANCZOS)
        out += encode_dxt1(level)

    sec1 = bytearray(c.section1)
    sec1[t.data_offset : t.data_offset + len(out)] = out
    c.section1 = bytes(sec1)


def list_textures(path: str | Path) -> tuple[SmbContainer, list[TextureEntry]]:
    c = SmbContainer.parse_file(path)
    return c, find_textures(c)
