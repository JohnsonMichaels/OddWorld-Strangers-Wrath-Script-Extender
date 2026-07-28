"""Measure the variable region between archive path and 0xBEEF1234 sentinel across all SMBs."""
import struct
from collections import Counter
from pathlib import Path

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\data")
SENTINEL = struct.pack("<I", 0xBEEF1234)

gap_u32_counts = Counter()
examples = {}

for p in sorted(GAME.rglob("*.smb")):
    size = p.stat().st_size
    with p.open("rb") as f:
        head = f.read(0x4000)
    magic, version, pathlen = struct.unpack_from("<III", head, 0)
    if magic != 0x3A4B5C6D:
        continue
    o = 12 + pathlen
    idx = head.find(SENTINEL, o)
    if idx < 0:
        gap_u32_counts["no-sentinel"] += 1
        continue
    n_u32 = (idx - o) / 4
    gap_u32_counts[n_u32] += 1
    if n_u32 not in examples:
        vals = struct.unpack_from(f"<{int(n_u32)}I", head, o) if n_u32 == int(n_u32) else ()
        examples[n_u32] = (p.name, size, vals)

print("u32s between path-end and sentinel -> file count")
for k, v in sorted(gap_u32_counts.items(), key=lambda kv: str(kv[0])):
    print(f"  {k}: {v}")
print("\nexamples:")
for k in sorted(examples, key=lambda x: float(x) if isinstance(x, (int, float)) else 999):
    name, size, vals = examples[k]
    print(f"  n={k}  {name} (size {size:,})")
    print(f"       {vals}")
