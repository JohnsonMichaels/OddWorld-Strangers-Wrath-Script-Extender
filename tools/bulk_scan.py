"""Scan every SMB/lvl/sbl file in the game for .foo script references and gameplay keywords."""
import re
import sys
from pathlib import Path

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\data")
KEYWORDS = [b".foo", b"health", b"Health", b"damage", b"Damage", b"reload", b"Reload", b"ammo", b"Ammo", b"HitPoints", b"hitpoints"]

results = []
for path in sorted(GAME.rglob("*")):
    if path.suffix.lower() not in (".smb", ".lvl", ".sbl", ".smh", ".bin", ".h", ".txt"):
        continue
    data = path.read_bytes()
    hits = {}
    for kw in KEYWORDS:
        n = data.count(kw)
        if n:
            hits[kw.decode()] = n
    if hits:
        results.append((path.relative_to(GAME), len(data), hits))

results.sort(key=lambda r: -sum(r[2].values()))
print(f"{len(results)} files with hits\n")
for rel, size, hits in results[:40]:
    print(f"{str(rel):70s} {size//1024:>8} KB  {hits}")
