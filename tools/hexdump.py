"""Plain hexdump of a file region."""
import sys
from pathlib import Path


def main(path: Path, start: int, end: int) -> None:
    data = Path(path).read_bytes()[start:end]
    for i in range(0, len(data), 16):
        row = data[i : i + 16]
        hexs = " ".join(f"{b:02X}" for b in row)
        asc = "".join(chr(b) if 32 <= b <= 126 else "." for b in row)
        print(f"{start + i:08X}  {hexs:<48}  {asc}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0))
