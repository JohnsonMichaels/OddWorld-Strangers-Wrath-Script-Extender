"""Validate: file_size == header_block + section1 + section2 for all SMB archives."""
import struct
from collections import Counter
from pathlib import Path

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\data")

ok = fail = 0
fails = []
h_values = Counter()
for p in GAME.rglob("*.smb"):
    size = p.stat().st_size
    with p.open("rb") as f:
        head = f.read(0x200)
    magic, ver, plen = struct.unpack_from("<III", head, 0)
    if magic != 0x3A4B5C6D:
        continue
    A, B, C, D, E, F, G, H = struct.unpack_from("<8I", head, 12 + plen)
    h_values[H] += 1
    if A + C + D == size:
        ok += 1
    else:
        fail += 1
        if len(fails) < 6:
            fails.append((p.name, size, A, C, D, "diff", size - A - C - D))

print(f"model file_size == hdr(A) + sec1(C) + sec2(D):  OK={ok}  FAIL={fail}")
for f in fails:
    print("  ", f)
print("field H distribution:", dict(h_values))
