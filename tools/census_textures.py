"""Census: how many texture entries exist across the whole game?

Answers "how much is left to do" for the HD texture project. The exported
pools only contain textures SWSE actually SAW being uploaded, which covers
only the areas that have been played. This walks the archives instead, so it
counts what exists rather than what has been visited.

Counts UNIQUE texture names, not entries: the same texture appears in many
level archives and must only be replaced once.

A running game holds the current level's archive open exclusively, so locked
files are skipped and reported rather than aborting the run.

USAGE
  python tools/census_textures.py [--min-size 128] [--out names.txt]
"""
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from oddforge.container import SmbContainer, SmbError
from oddforge.toc import find_textures

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\data")


def main() -> int:
    args = sys.argv[1:]
    min_size = 0
    out_path = None
    i = 0
    while i < len(args):
        if args[i] == "--min-size" and i + 1 < len(args):
            min_size = int(args[i + 1]); i += 2
        elif args[i] == "--out" and i + 1 < len(args):
            out_path = Path(args[i + 1]); i += 2
        else:
            i += 1

    entries = 0
    archives_with = 0
    locked = []
    unreadable = 0
    fmt_counts = Counter()
    size_counts = Counter()
    # name -> (fmt, w, h). Same texture in many archives collapses to one.
    unique: dict[str, tuple[str, int, int]] = {}

    files = sorted(GAME.rglob("*.smb"))
    for n, p in enumerate(files):
        if n % 200 == 0:
            print(f"  ...{n}/{len(files)}", flush=True)
        try:
            c = SmbContainer.parse_file(p)
        except SmbError:
            unreadable += 1
            continue
        except (PermissionError, OSError):
            locked.append(p.name)      # game has it open; not an error
            continue
        texs = find_textures(c)
        if not texs:
            continue
        archives_with += 1
        entries += len(texs)
        for t in texs:
            fmt_counts[t.fmt] += 1
            size_counts[f"{t.width}x{t.height}"] += 1
            unique[t.name.rsplit("\\", 1)[-1].upper()] = (t.fmt, t.width, t.height)

    big = {k: v for k, v in unique.items() if min(v[1], v[2]) >= min_size}

    print(f"\narchives scanned      : {len(files)}")
    print(f"  containing textures : {archives_with}")
    print(f"  locked (game open)  : {len(locked)}{'  ' + ', '.join(locked[:4]) if locked else ''}")
    print(f"  unparseable         : {unreadable}")
    print(f"\ntexture entries       : {entries}")
    print(f"UNIQUE textures       : {len(unique)}")
    if min_size:
        print(f"  >= {min_size}px on both axes : {len(big)}")
    print(f"\nformats: {dict(sorted(fmt_counts.items()))}")
    print("\ntop sizes:")
    for size, count in size_counts.most_common(12):
        print(f"   {size:>12}  {count}")

    if out_path:
        sel = big if min_size else unique
        out_path.write_text("\n".join(sorted(sel)), encoding="utf-8")
        print(f"\nwrote {out_path} ({len(sel)} names)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
