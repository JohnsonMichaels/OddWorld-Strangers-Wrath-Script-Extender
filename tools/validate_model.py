"""Validate the SMB header model against every archive in the game.

Model so far:
  u32 magic          (0x3A4B5C6D)
  u32 version        (5?)
  u32 pathlen + path (archive's own path, no null)
  u32 header_block_size   } expect header_block_size + data_block_size == file size
  u32 toc_used_bytes      } actual used bytes in header block?
  u32 data_block_size
  u32 unk0
  u32 payload_main_size?
  u32 unk1
  u32 entry_count_a
  u32 entry_count_b
  u32 sentinel (0xBEEF1234)
"""
import struct
from collections import Counter
from pathlib import Path

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\data")

ok = 0
bad_magic = []
bad_size = []
versions = Counter()
sentinels = Counter()
counts_seen = Counter()

files = sorted(GAME.rglob("*.smb")) + sorted(GAME.rglob("*.smh"))
for p in files:
    size = p.stat().st_size
    with p.open("rb") as f:
        head = f.read(0x200)
    if len(head) < 0x60:
        bad_magic.append((p, "too small"))
        continue
    magic, version, pathlen = struct.unpack_from("<III", head, 0)
    if magic != 0x3A4B5C6D:
        bad_magic.append((p, hex(magic)))
        continue
    versions[version] += 1
    o = 12 + pathlen
    try:
        hdr_block, toc_used, data_block, unk0, payload, unk1, cnt_a, cnt_b, sentinel = struct.unpack_from("<9I", head, o)
    except struct.error:
        bad_magic.append((p, "short header"))
        continue
    sentinels[hex(sentinel)] += 1
    counts_seen[(cnt_a, cnt_b)] += 1
    if hdr_block + data_block == size:
        ok += 1
    else:
        bad_size.append((p.name, size, hdr_block, data_block))

print(f"total archives: {len(files)}")
print(f"magic OK + size model OK: {ok}")
print(f"bad magic: {len(bad_magic)}  e.g. {bad_magic[:5]}")
print(f"size model FAILS: {len(bad_size)}  e.g. {bad_size[:8]}")
print(f"versions: {dict(versions)}")
print(f"sentinel values: {dict(list(sentinels.items())[:6])}")
print(f"top (count_a,count_b): {counts_seen.most_common(10)}")
