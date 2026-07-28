"""Map the structure of an SMB archive: find length-prefixed strings and the gaps between them.

A "length-prefixed string" here is: uint32 LE length N, followed by N-1 printable
ASCII bytes and a null (or N printable bytes) - the pattern seen in the header.
Everything between such strings is unknown binary; we print gap sizes so we can
start assigning meaning to fixed-size records.
"""
import struct
import sys
from pathlib import Path


def is_printable(b: bytes) -> bool:
    return all(32 <= c <= 126 for c in b)


def find_strings(data: bytes):
    """Yield (offset_of_length_field, string, total_span) for every plausible length-prefixed string."""
    i = 0
    n = len(data)
    while i + 4 <= n:
        (length,) = struct.unpack_from("<I", data, i)
        if 4 <= length <= 260 and i + 4 + length <= n:
            chunk = data[i + 4 : i + 4 + length]
            if is_printable(chunk):
                yield i, chunk.decode("ascii"), 4 + length
                i += 4 + length
                continue
        i += 1


def main(path: Path, limit: int) -> None:
    data = path.read_bytes()
    print(f"{path.name}: {len(data):,} bytes")
    prev_end = 0
    count = 0
    for off, s, span in find_strings(data):
        gap = off - prev_end
        print(f"@0x{off:08X} gap={gap:>6}  len={span-4:>4}  {s[:100]}")
        prev_end = off + span
        count += 1
        if count >= limit:
            print("... (limit reached)")
            break
    print(f"\n{count} strings shown; trailing bytes after last string: {len(data) - prev_end}")


if __name__ == "__main__":
    main(Path(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 80)
