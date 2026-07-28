"""Extract (entry_name, record_bytes) pairs from SMB TOCs and compare record layouts.

Uses the known container layout: records are the bytes between the end of one
length-prefixed name string and the start of the next (or end of TOC for the last).
"""
import struct
import sys
from pathlib import Path


def is_printable(b: bytes) -> bool:
    return all(32 <= c <= 126 for c in b)


def parse(path: Path):
    data = path.read_bytes()
    magic, version, plen = struct.unpack_from("<III", data, 0)
    assert magic == 0x3A4B5C6D, hex(magic)
    o = 12 + plen
    A, B, C, D, E, F, G, H = struct.unpack_from("<8I", data, o)
    toc = data[: B]  # TOC = used part of header block
    # find name strings within the TOC (skip the self-path at the start)
    names = []  # (len_field_offset, name, end_offset)
    i = o + 32 + 4  # after the 8 fields + sentinel
    while i + 4 <= len(toc):
        (length,) = struct.unpack_from("<I", toc, i)
        if 4 <= length <= 260 and i + 4 + length <= len(toc):
            chunk = toc[i + 4 : i + 4 + length]
            if is_printable(chunk) and (b"\\" in chunk or b"." in chunk):
                names.append((i, chunk.decode("ascii"), i + 4 + length))
                i += 4 + length
                continue
        i += 1
    return dict(size=len(data), A=A, B=B, C=C, D=D, E=E, F=F, G=G, H=H,
                toc=toc, names=names, preamble_start=o + 36)


def show(path: Path):
    r = parse(path)
    print(f"### {path.name}  size={r['size']:,}  A={r['A']:#x} B={r['B']:#x} "
          f"C={r['C']:#x} D={r['D']:#x} E={r['E']:#x} F={r['F']:#x} G={r['G']} H={r['H']}")
    names = r["names"]
    toc = r["toc"]
    pre = toc[r["preamble_start"] : names[0][0]] if names else b""
    print(f"preamble ({len(pre)}B): {pre.hex(' ')}")
    for idx, (off, name, end) in enumerate(names):
        rec_end = names[idx + 1][0] if idx + 1 < len(names) else len(toc)
        rec = toc[end:rec_end]
        print(f"[{idx}] {name}")
        print(f"    record {len(rec)}B: {rec.hex(' ')}")
    print()


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        show(Path(arg))
