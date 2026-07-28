"""Collect every length-prefixed identifier string across all SMB TOCs.

The engine's reflective serializer writes field/class names as length-prefixed
strings (seen: m_min, m_max, "class CClassDef<class Vec3>"). This scan builds the
global vocabulary so we can find stat fields (health, damage, speed, ammo...).
"""
import re
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from oddforge.container import SmbContainer, SmbError

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\data")

IDENT_RE = re.compile(r"^[A-Za-z_{][A-Za-z0-9_ <>(){}:.|-]*$")

vocab = Counter()          # identifier -> total occurrences
where = {}                 # identifier -> first archive seen

for p in sorted(GAME.rglob("*.smb")):
    try:
        c = SmbContainer.parse_file(p)
    except SmbError:
        continue
    toc = c.toc
    i = 0
    n = len(toc)
    while i + 4 <= n:
        (length,) = struct.unpack_from("<I", toc, i)
        if 3 <= length <= 64 and i + 4 + length <= n:
            chunk = toc[i + 4 : i + 4 + length]
            if all(32 <= b <= 126 for b in chunk):
                s = chunk.decode("ascii")
                if IDENT_RE.match(s) and not s.startswith("\\"):
                    vocab[s] += 1
                    where.setdefault(s, p.name)
                    i += 4 + length
                    continue
        i += 1

print(f"unique identifiers: {len(vocab)}\n")

KEY = ("health", "damage", "speed", "ammo", "reload", "hit", "armor",
       "stamina", "moolah", "bounty", "weapon", "rate", "range", "accuracy")
hits = [(s, c) for s, c in vocab.items() if any(k in s.lower() for k in KEY)]
hits.sort(key=lambda x: -x[1])
print(f"stat-like identifiers ({len(hits)}):")
for s, cnt in hits[:60]:
    print(f"  {cnt:>6}  {s:50}  (first: {where[s]})")

print("\nmost common identifiers overall:")
for s, cnt in vocab.most_common(40):
    print(f"  {cnt:>6}  {s}")
