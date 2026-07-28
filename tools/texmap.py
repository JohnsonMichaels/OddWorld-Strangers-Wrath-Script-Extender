"""Map every texture in the game to its SWSE runtime fingerprint.

WHY THIS EXISTS
    The HD project had two lists that could not be compared. The archives know
    texture NAMES (\\art\\textures\\rocks\\wall01.tga); the runtime replacement
    system knows FINGERPRINTS (A1B2C3D4.oft), hashed from pixel data. So
    "which textures are still vanilla?" could only be answered by guessing from
    the export folders, which only ever contained textures that happened to be
    uploaded while someone was playing.

    This computes the runtime fingerprint from the ARCHIVE side, so the two
    lists join exactly. Answers "what is left" for the whole game, not just the
    parts that have been visited.

THE FINGERPRINT  (must stay in sync with Fnv1a + the hash line in swse/glspy.cpp)
    hash = fnv1a32(level0_data[:4096]) ^ (w * 73856093) ^ (h * 19349663)

    with the same gates glspy applies before it will substitute at all:
    level 0 only, data size >= 64 bytes, width >= 64. Textures failing those
    gates can never be replaced and are excluded from the totals.

USAGE
  python tools/texmap.py [--out remaining.csv] [--min-size N] [--live <dir>]
                         [--match REGEX] [--fp-out fingerprints.txt]

  Prints the live/remaining split and writes the remaining textures (name,
  size, fingerprint, archive) so a batch can be picked by what it IS rather
  than by which folder it landed in.

  --match/--fp-out emit the fingerprints of textures whose NAME matches a
  regex, for features that need to recognise a class of texture at runtime.
  This is how foliage is identified for the wind effect: SWSE only ever sees
  pixel data, so the artists' names (bamboo_leaves_AT, cedar_fir_tree_01_AT)
  have to be turned into fingerprints offline. Matching runs over ALL
  textures, live or not - the game uploads vanilla data and glspy hashes it
  before any HD substitution, so the vanilla fingerprint is the right key
  even for textures already replaced.
"""
from __future__ import annotations

import csv
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from oddforge.container import SmbContainer, SmbError
from oddforge.toc import find_textures

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath")
DATA = GAME / "data"
LIVE = GAME / "SWSEMods" / "SWSE HD" / "textures"

MASK = 0xFFFFFFFF

# Level-0 byte size per texture format. Only block-compressed formats reach
# glCompressedTexImage2D, which is the only upload path glspy fingerprints, so
# uncompressed formats can never be matched at runtime and are left out.
#   12 = DXT1  (verified)          8 bytes / 4x4 block
#   14/15      DXT3 / DXT5        16 bytes / 4x4 block  -- inferred, and the
#              inference is SELF-CHECKING: if the size were wrong the computed
#              fingerprints would not match anything the game uploaded, and
#              --fp-in would identify nothing.
BLOCK_BYTES = {12: 8, 14: 16, 15: 16}


def level0_size(fmt: int, w: int, h: int) -> int | None:
    bb = BLOCK_BYTES.get(fmt)
    if bb is None:
        return None
    return max(w // 4, 1) * max(h // 4, 1) * bb


def fnv1a32(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & MASK
    return h


def fingerprint(level0: bytes, w: int, h: int) -> int:
    return (fnv1a32(level0[:4096]) ^ (w * 73856093) ^ (h * 19349663)) & MASK


def main() -> int:
    args = sys.argv[1:]
    out_path = Path("texmap_remaining.csv")
    min_size = 0
    live_dir = LIVE
    match_re = None
    fp_out = None
    fp_in = None
    i = 0
    while i < len(args):
        if args[i] == "--out" and i + 1 < len(args):
            out_path = Path(args[i + 1]); i += 2
        elif args[i] == "--min-size" and i + 1 < len(args):
            min_size = int(args[i + 1]); i += 2
        elif args[i] == "--live" and i + 1 < len(args):
            live_dir = Path(args[i + 1]); i += 2
        elif args[i] == "--match" and i + 1 < len(args):
            match_re = re.compile(args[i + 1], re.I); i += 2
        elif args[i] == "--fp-out" and i + 1 < len(args):
            fp_out = Path(args[i + 1]); i += 2
        elif args[i] == "--fp-in" and i + 1 < len(args):
            fp_in = Path(args[i + 1]); i += 2
        else:
            i += 1

    live: set[int] = set()
    if live_dir.is_dir():
        for p in live_dir.glob("*.oft"):
            try:
                live.add(int(p.stem, 16))
            except ValueError:
                continue
    print(f"live .oft files: {len(live)}  ({live_dir})")

    # fingerprint -> (name, w, h, archive). First archive wins, matching the
    # loader's own first-wins rule for duplicate texture paths.
    seen: dict[int, tuple[str, int, int, str]] = {}
    # every fingerprintable format, for name lookup (--fp-in / --match)
    allfmt: dict[int, tuple[str, int, int, str]] = {}
    locked: list[str] = []
    gated = 0
    non_dxt1 = 0

    files = sorted(DATA.rglob("*.smb"))
    for n, p in enumerate(files):
        if n % 200 == 0:
            print(f"  ...{n}/{len(files)}", flush=True)
        try:
            c = SmbContainer.parse_file(p)
        except SmbError:
            continue
        except (PermissionError, OSError):
            locked.append(p.name)
            continue

        for t in find_textures(c):
            size = level0_size(t.fmt, t.width, t.height)
            if size is None:
                non_dxt1 += 1
                continue
            # the gates glspy applies before it will consider a substitution
            if size < 64 or t.width < 64:
                gated += 1
                continue
            data = c.section1[t.data_offset : t.data_offset + size]
            if len(data) < size:
                continue
            fp = fingerprint(data, t.width, t.height)
            if fp not in allfmt:
                allfmt[fp] = (t.name.rsplit("\\", 1)[-1], t.width, t.height, p.name)
            # Only DXT1 can actually be REPLACED by the current pipeline, so the
            # live/remaining accounting stays DXT1-only even though the
            # fingerprint index covers more.
            if t.is_dxt1 and fp not in seen:
                seen[fp] = (t.name.rsplit("\\", 1)[-1], t.width, t.height, p.name)

    have = {fp: v for fp, v in seen.items() if fp in live}
    left = {fp: v for fp, v in seen.items() if fp not in live}
    if min_size:
        left = {fp: v for fp, v in left.items()
                if min(v[1], v[2]) >= min_size}

    print(f"\narchives scanned        : {len(files)}")
    if locked:
        print(f"  LOCKED (game running) : {len(locked)}  -> totals are a FLOOR")
    print(f"non-DXT1 entries skipped: {non_dxt1}")
    print(f"below substitution gate : {gated}")
    print(f"\nreplaceable textures    : {len(seen)}")
    print(f"  already HD (live)     : {len(have)}")
    print(f"  REMAINING             : {len(left)}"
          + (f"  (>= {min_size}px)" if min_size else ""))
    unmatched = len(live) - len(have)
    if unmatched:
        print(f"  live files with no archive match: {unmatched}"
              " (locked archives, or stale .oft)")

    sizes = Counter(f"{v[1]}x{v[2]}" for v in left.values())
    print("\nremaining by size:")
    for s, ct in sizes.most_common(12):
        print(f"   {s:>12}  {ct}")

    if fp_in is not None:
        # Turn a runtime fingerprint list back into artist names. This is what
        # makes "what is actually on screen" answerable, instead of guessing
        # which words a texture might have been named with.
        want = []
        for line in fp_in.read_text(encoding="utf-8", errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            tok = line.split()[0]
            try:
                want.append(int(tok, 16))
            except ValueError:
                continue
        named = [(fp, allfmt[fp]) for fp in want if fp in allfmt]
        unknown = [fp for fp in want if fp not in allfmt]
        print(f"\n--fp-in: {len(want)} fingerprint(s), {len(named)} identified,"
              f" {len(unknown)} unknown")
        for fp, (name, w, h, arch) in sorted(named, key=lambda kv: kv[1][0]):
            print(f"   {fp:08X}  {name:<38} {w}x{h}  {arch}")
        if unknown:
            print(f"   ({len(unknown)} not found in archives - locked files,"
                  " non-DXT1, or generated at runtime)")

    if match_re is not None:
        hits = {fp: v for fp, v in allfmt.items() if match_re.search(v[0])}
        print(f"\n--match: {len(hits)} texture(s) matched")
        for fp, (name, w, h, arch) in sorted(hits.items(), key=lambda kv: kv[1][0])[:20]:
            print(f"   {fp:08X}  {name}  {w}x{h}")
        if len(hits) > 20:
            print(f"   ... and {len(hits) - 20} more")
        if fp_out:
            lines = ["# SWSE texture fingerprint list - generated by tools/texmap.py",
                     f"# pattern: {match_re.pattern}",
                     "# one 8-digit hex fingerprint per line; '#' starts a comment"]
            lines += [f"{fp:08X}  {v[0]}" for fp, v in
                      sorted(hits.items(), key=lambda kv: kv[1][0])]
            fp_out.write_text("\n".join(lines) + "\n", encoding="utf-8")
            print(f"wrote {fp_out} ({len(hits)} fingerprints)")

    with out_path.open("w", newline="", encoding="utf-8") as fh:
        wr = csv.writer(fh)
        wr.writerow(["fingerprint", "name", "width", "height", "archive"])
        for fp, (name, w, h, arch) in sorted(
                left.items(), key=lambda kv: (-kv[1][1] * kv[1][2], kv[1][0])):
            wr.writerow([f"{fp:08X}", name, w, h, arch])
    print(f"\nwrote {out_path} ({len(left)} rows, largest first)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
