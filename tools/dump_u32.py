"""Dump a byte range of a file as little-endian uint32s (with float/ASCII hints)."""
import struct
import sys
from pathlib import Path


def main(path: Path, start: int, end: int) -> None:
    data = path.read_bytes()
    print(f"{path.name} [{start:#x}:{end:#x}]  (file size {len(data):#x} = {len(data):,})")
    for off in range(start, min(end, len(data) - 3), 4):
        (u,) = struct.unpack_from("<I", data, off)
        (f,) = struct.unpack_from("<f", data, off)
        raw = data[off : off + 4]
        ascii_hint = "".join(chr(c) if 32 <= c <= 126 else "." for c in raw)
        fhint = f"{f:.4g}" if 1e-6 < abs(f) < 1e9 else ""
        print(f"0x{off:06X}: {u:>12}  0x{u:08X}  {ascii_hint}  {fhint}")


if __name__ == "__main__":
    main(Path(sys.argv[1]), int(sys.argv[2], 0), int(sys.argv[3], 0))
