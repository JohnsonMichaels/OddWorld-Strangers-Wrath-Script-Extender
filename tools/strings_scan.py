"""Scan SMB archives for readable ASCII strings - first look at what's inside."""
import re
import sys
from collections import Counter
from pathlib import Path

MIN_LEN = 5
STRING_RE = re.compile(rb"[\x20-\x7e]{%d,}" % MIN_LEN)


def scan(path: Path, keywords: list[str]) -> None:
    data = path.read_bytes()
    strings = [m.group().decode("ascii") for m in STRING_RE.finditer(data)]
    print(f"=== {path.name} ({len(data):,} bytes, {len(strings)} strings) ===")

    exts = Counter()
    for s in strings:
        m = re.search(r"\.([a-zA-Z0-9]{2,4})$", s)
        if m:
            exts[m.group(1).lower()] += 1
    print("file-extension mentions:", dict(exts.most_common(15)))

    hits = [s for s in strings if any(k in s.lower() for k in keywords)]
    print(f"keyword hits ({len(hits)}):")
    for s in hits[:40]:
        print("   ", s[:120])
    print()


if __name__ == "__main__":
    keywords = ["health", "damage", "ammo", "reload", "speed", "script", ".foo", "hitpoint", "weapon"]
    for arg in sys.argv[1:]:
        scan(Path(arg), keywords)
