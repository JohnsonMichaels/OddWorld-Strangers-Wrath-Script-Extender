"""SMB container layer: the outermost archive structure.

Layout (validated against all 1,222 archives - see FORMAT.md):

    u32     magic = 0x3A4B5C6D
    u32     version = 5
    u32+str self_path (length-prefixed ASCII, no null, byte-packed)
    u32 x8  header_block_size (A), toc_used (B), sec1_size (C), sec2_size (D),
            sec1_used (E), sec2_used (F), entry_count (G), section_count (H)
    ...     TOC (starts with 0xBEEF1234 sentinel), runs to offset B
    ...     padding to offset A
    bytes   section1  [A, A+C)
    bytes   section2  [A+C, A+C+D)

Invariant: file_size == A + C + D
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = 0x3A4B5C6D
SENTINEL = 0xBEEF1234


class SmbError(ValueError):
    pass


@dataclass
class SmbContainer:
    version: int
    self_path: str
    header_block_size: int   # A
    toc_used: int            # B (absolute file offset where TOC ends)
    sec1_size: int           # C
    sec2_size: int           # D
    sec1_used: int           # E
    sec2_used: int           # F
    entry_count: int         # G
    section_count: int       # H
    toc: bytes               # from after the 8 header fields to offset B (starts with sentinel)
    header_padding: bytes    # [B, A) - usually zeros, preserved verbatim
    section1: bytes
    section2: bytes

    # ------------------------------------------------------------------ parse
    @classmethod
    def parse(cls, data: bytes) -> "SmbContainer":
        if len(data) < 16:
            raise SmbError("file too small")
        magic, version, pathlen = struct.unpack_from("<III", data, 0)
        if magic != MAGIC:
            raise SmbError(f"bad magic {magic:#x} (expected {MAGIC:#x})")
        o = 12
        self_path = data[o : o + pathlen].decode("ascii")
        o += pathlen
        A, B, C, D, E, F, G, H = struct.unpack_from("<8I", data, o)
        o += 32
        if A + C + D != len(data):
            raise SmbError(f"size invariant broken: {A}+{C}+{D} != {len(data)}")
        (sent,) = struct.unpack_from("<I", data, o)
        if sent != SENTINEL:
            raise SmbError(f"sentinel not found at {o:#x}: {sent:#x}")
        toc = data[o:B]
        header_padding = data[B:A]
        section1 = data[A : A + C]
        section2 = data[A + C : A + C + D]
        return cls(version, self_path, A, B, C, D, E, F, G, H,
                   toc, header_padding, section1, section2)

    @classmethod
    def parse_file(cls, path: str | Path) -> "SmbContainer":
        return cls.parse(Path(path).read_bytes())

    # ------------------------------------------------------------------ build
    def build(self) -> bytes:
        path_bytes = self.self_path.encode("ascii")
        out = bytearray()
        out += struct.pack("<III", MAGIC, self.version, len(path_bytes))
        out += path_bytes
        out += struct.pack(
            "<8I",
            self.header_block_size, self.toc_used,
            self.sec1_size, self.sec2_size,
            self.sec1_used, self.sec2_used,
            self.entry_count, self.section_count,
        )
        out += self.toc
        out += self.header_padding
        out += self.section1
        out += self.section2
        return bytes(out)
