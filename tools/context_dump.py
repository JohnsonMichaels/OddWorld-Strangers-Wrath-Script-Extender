"""Show surrounding context for keyword occurrences inside a binary file."""
import re
import sys
from pathlib import Path


def printable(b: bytes) -> str:
    return "".join(chr(c) if 32 <= c <= 126 else "." for c in b)


def dump(path: Path, keyword: bytes, before: int, after: int, limit: int) -> None:
    data = path.read_bytes()
    print(f"=== {path.name}: first {limit} occurrences of {keyword!r} ===")
    start = 0
    for _ in range(limit):
        i = data.find(keyword, start)
        if i < 0:
            break
        lo, hi = max(0, i - before), min(len(data), i + after)
        print(f"@0x{i:08X}: {printable(data[lo:hi])}")
        start = i + 1
    print()


if __name__ == "__main__":
    path = Path(sys.argv[1])
    kw = sys.argv[2].encode()
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    dump(path, kw, 60, 120, limit)
