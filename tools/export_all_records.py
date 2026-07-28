"""Bulk-export every archive's records to readable, editable text files.

Mirrors the texture dump workflow: dump -> read/edit -> re-import.

    python tools\\export_all_records.py "<game>\\data" "C:\\RecordDump"

Produces one .txt per archive, preserving the archive folder structure, so
you can browse NPCs, weapons, effects and level objects as plain numbers.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from oddforge.container import SmbContainer
from oddforge.records import export_records, find_records


def main(argv=None):
    argv = argv or sys.argv[1:]
    if len(argv) < 2:
        print(__doc__)
        return 1
    src, out = Path(argv[0]), Path(argv[1])
    archives = sorted(src.rglob("*.smb"))
    print(f"{len(archives)} archives to scan\n")

    total_recs = 0
    written = 0
    skipped = 0
    for i, smb in enumerate(archives, 1):
        try:
            c = SmbContainer.parse_file(smb)
            recs = [r for r in find_records(c) if r.values]
        except Exception:
            skipped += 1
            continue
        if not recs:
            continue
        rel = smb.relative_to(src).with_suffix(".txt")
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            n = export_records(smb, dest)
        except Exception:
            skipped += 1
            continue
        total_recs += len(recs)
        written += 1
        if i % 100 == 0 or written < 5:
            print(f"  [{i}/{len(archives)}] {rel}  ({len(recs)} records)")

    print(f"\nwrote {written} dump files, {total_recs} records total")
    print(f"skipped/unreadable: {skipped}")
    print(f"output: {out.resolve()}")
    print("\nEdit any number in a dump, then re-import that archive:")
    print("  python -m oddforge.records import <archive.smb> <dump.txt>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
