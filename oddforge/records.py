"""Generic game-record export/import - the data-side counterpart to textures.py.

Archives store records as a flat sequence in the TOC:

    [40-byte node header][u32 name_len][name bytes][field data ...]  repeated

The header begins with the build timestamp 0x4DFAA77E, which makes records
findable without knowing any type information. Field data runs until the next
header, and is a packed array of 32-bit values (floats and ints).

This gives us the same round-trip we have for textures:
    find_records()   -> enumerate every record
    export_records() -> human-readable .txt dump you can edit
    apply_edits()    -> write edited values back into the archive

Verified against known data: the bounty record "Fatty McBoomboom" decodes to
1000.0 / 500.0 (its alive/dead moolah values).
"""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

from .container import SmbContainer

TIMESTAMP = 0x4DFAA77E
HDR_SIZE = 40


@dataclass
class Record:
    hdr_off: int          # offset of the 40-byte header in the TOC
    node_id: int
    name: str
    data_off: int         # offset of the field data in the TOC
    data_len: int
    values: list          # [(offset, kind, value)] kind in {"f","i"}

    @property
    def floats(self):
        return [(o, v) for o, k, v in self.values if k == "f"]


def _plausible_float(f: float) -> bool:
    a = abs(f)
    return a == 0.0 or (1e-4 < a < 1e7)


def find_records(c: SmbContainer) -> list[Record]:
    """Enumerate every record in the archive's TOC."""
    toc = c.toc
    n = len(toc)
    marker = struct.pack("<I", TIMESTAMP)

    # locate all header positions first
    heads = []
    pos = 0
    while True:
        j = toc.find(marker, pos)
        if j < 0:
            break
        pos = j + 1
        if j + HDR_SIZE + 4 > n:
            continue
        ln = struct.unpack_from("<I", toc, j + HDR_SIZE)[0]
        if not (1 <= ln <= 260) or j + HDR_SIZE + 4 + ln > n:
            continue
        raw = toc[j + HDR_SIZE + 4: j + HDR_SIZE + 4 + ln]
        if not all(32 <= b <= 126 for b in raw):
            continue
        heads.append((j, ln, raw.decode("latin1")))

    # Some archives (global_prefs.smb, sec1=0) hold their records entirely in
    # the TOC without the 40-byte timestamp headers. Fall back to scanning
    # length-prefixed names and treating the bytes up to the next name as the
    # record's fields - this is where the real tunables live (bounty payouts,
    # EffectMixDef values, etc).
    # An archive with no section1 stores everything in the TOC in the flat
    # layout (a few stray timestamp matches can still occur, so key off the
    # missing data section rather than off `heads` being empty).
    if not c.section1 or len(heads) < 5:
        flat = _find_records_flat(c)
        if len(flat) > len(heads):
            return flat

    out = []
    for i, (j, ln, name) in enumerate(heads):
        vals = struct.unpack_from("<10I", toc, j)
        data_off = j + HDR_SIZE + 4 + ln
        data_end = heads[i + 1][0] if i + 1 < len(heads) else n
        data_len = max(0, data_end - data_off)

        values = []
        for k in range(0, data_len - 3, 4):
            raw4 = toc[data_off + k: data_off + k + 4]
            u, = struct.unpack("<I", raw4)
            f, = struct.unpack("<f", raw4)
            if _plausible_float(f) and u not in (0,):
                values.append((data_off + k, "f", f))
            else:
                values.append((data_off + k, "i", u))
        out.append(Record(hdr_off=j, node_id=vals[2], name=name,
                          data_off=data_off, data_len=data_len, values=values))
    return out


def _find_records_flat(c: SmbContainer) -> list[Record]:
    """Header-less TOC layout: [u32 len][name][field data] repeating.

    Used by archives like global_prefs.smb which carry no section1 and no
    timestamp headers. Verified against known data: the bounty record
    'Fatty McBoomboom' decodes to 1000.0 / 500.0, its real payout values.
    """
    toc = c.toc
    n = len(toc)
    names = []
    i = 0
    while i + 4 < n:
        ln = struct.unpack_from("<I", toc, i)[0]
        if 3 <= ln <= 200 and i + 4 + ln <= n:
            s = toc[i + 4: i + 4 + ln]
            if all(32 <= b <= 126 for b in s):
                names.append((i, ln, s.decode("latin1")))
                i += 4 + ln
                continue
        i += 1

    out = []
    for k, (off, ln, name) in enumerate(names):
        data_off = off + 4 + ln
        data_end = names[k + 1][0] if k + 1 < len(names) else n
        data_len = max(0, min(data_end - data_off, 512))
        values = []
        for m in range(0, data_len - 3, 4):
            raw4 = toc[data_off + m: data_off + m + 4]
            u, = struct.unpack("<I", raw4)
            f, = struct.unpack("<f", raw4)
            if _plausible_float(f) and u != 0:
                values.append((data_off + m, "f", f))
            else:
                values.append((data_off + m, "i", u))
        out.append(Record(hdr_off=off, node_id=0, name=name,
                          data_off=data_off, data_len=data_len, values=values))
    return out


# ---------------------------------------------------------------- labelling
# The archives keep field VALUES but not field NAMES. The exe's reflection
# schema (swse/research/REFLECTION_SCHEMA.md) has the names, grouped by class.
# We match a record to a class and then label its values positionally.
_SCHEMA_CACHE: dict | None = None


def load_schema() -> dict:
    """class name -> [field names], parsed from the reflection schema dump."""
    global _SCHEMA_CACHE
    if _SCHEMA_CACHE is not None:
        return _SCHEMA_CACHE
    # When frozen by PyInstaller the source tree isn't on disk - the schema is
    # bundled next to the exe's temp extraction root (sys._MEIPASS).
    import sys as _sys
    cands = []
    base = getattr(_sys, "_MEIPASS", None)
    if base:
        cands.append(Path(base) / "swse" / "research" / "REFLECTION_SCHEMA.md")
        cands.append(Path(base) / "REFLECTION_SCHEMA.md")
        cands.append(Path(_sys.executable).resolve().parent / "REFLECTION_SCHEMA.md")
    cands.append(Path(__file__).resolve().parent.parent / "swse" / "research"
                 / "REFLECTION_SCHEMA.md")
    md = next((p for p in cands if p.exists()), cands[-1])
    schema: dict[str, list[str]] = {}
    if md.exists():
        cur = None
        for line in md.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^##\s+(\S+)\s+\((\d+) fields\)", line)
            if m:
                cur = m.group(1)
                schema[cur] = []
                continue
            if cur and line.startswith("- "):
                schema[cur].extend(re.findall(r"`(m_[A-Za-z0-9_]+)`", line))
    _SCHEMA_CACHE = schema
    return schema


# Map a prefs FOLDER to its class. Substring matching was tried first and
# produced false positives - the journal line "Gutlips is stealing artifacts."
# matched ArtifactPrefs - so a record only gets a class if its name is an
# actual /data/prefs/<folder>/ path.
_FOLDER_CLASS = {
    "critterpaths":  "CritterPathPrefs",
    "crittercues":   "CritterCuePrefs",
    "explosions":    "ExplosionPrefs",
    "rumble":        "RumblePrefs",
    "artifacts":     "ArtifactPrefs",
    "bounty":        "BountyPrefs",
    "weapons":       "WeaponPrefs",
    "playerarmor":   "PlayerArmorPrefs",
}
_PREFS_RE = re.compile(r"[\\/]data[\\/]prefs[\\/]([^\\/]+)[\\/]", re.I)


# Asset-reference records (textures, geometry, animations) are NOT prefs
# records - their trailing bytes are stream/format data, not named fields.
# Labelling them produced confident nonsense (a .bmp matching
# SparkParticleSystem purely on field count), so they're excluded outright.
_ASSET_EXT = (".bmp", ".tga", ".dds", ".png", ".gr2", ".geo", ".msh",
              ".skl", ".anm", ".foo", ".wav", ".xsb", ".bik")


def guess_class(rec: "Record", schema: dict | None = None) -> str | None:
    """Class for a record, or None when we can't tell.

    Deliberately conservative: only a positive NAME match counts. Matching on
    field count alone gave false positives, and a wrong label is worse than no
    label - it tells a modder they're editing something they aren't.
    """
    schema = schema or load_schema()
    low = rec.name.lower()
    if low.endswith(_ASSET_EXT):
        return None
    def _resolve(cls_name):
        for k in schema:
            if k.lower() == cls_name.lower():
                return k
        return None

    # 1) a real /data/prefs/<folder>/ path
    m = _PREFS_RE.search(low)
    if m:
        cls = _FOLDER_CLASS.get(m.group(1))
        if cls:
            return _resolve(cls)

    # 2) characters/NPC prefs live in NPCPrefs (which is also where the bounty
    #    payout fields m_killMoolah/m_captureMoolah are defined)
    if "\\characters\\" in low or "/characters/" in low or low.endswith("prefs.txt"):
        return _resolve("NPCPrefs")

    # 3) bounty NPC records are named after the bounty and carry NPCPrefs
    #    fields - verified via Fatty McBoomboom's 1000/500 payouts.
    if rec.name in _BOUNTY_NAMES:
        return _resolve("NPCPrefs")
    return None


# Bounty NPCs seen in global_prefs.smb - their records are NPCPrefs.
_BOUNTY_NAMES = {
    "Fatty McBoomboom", "Blisterz Booty", "Boilz Booty", "Elboze Freely",
    "Filthy Hands Floyd", "Jo'momma", "Last Legs", "Lefty Lugnutz",
    "Lootin' Duke", "Meagley McGraw", "Packrat Palooka", "Sneaky Sally",
    "X'plosives Macgeer", "Eugene", "The Tooth Merchant", "Gutlips",
    "Buzzarton", "Sekto", "Bad Bart", "Looten Duke",
}


def label_record(rec: "Record", schema: dict | None = None) -> list[tuple]:
    """[(offset, kind, value, field_name_or_None)] for a record.

    Field ORDER is calibrated: the reflection string pool lists fields in the
    REVERSE of their record order. Established from a record whose meaning we
    know - Fatty McBoomboom reads 1000 (capture/alive) then 500 (kill/dead),
    while the schema lists m_killMoolah before m_captureMoolah. Hence the
    reversal below.

    ALIGNMENT is still approximate: many records are partial (only the fields
    that differ from defaults) and some begin with type-id dwords, so index 0
    won't always be the first schema field. Treat a name as a strong hint and
    confirm in-game. Editing by raw offset is always exact; only the NAME
    beside it may be off.
    """
    schema = schema or load_schema()
    cls = guess_class(rec, schema)
    fields = list(reversed(schema.get(cls, []))) if cls else []
    out = []
    for i, (off, kind, val) in enumerate(rec.values):
        name = fields[i] if i < len(fields) else None
        out.append((off, kind, val, name))
    return out


def export_records(path: str | Path, out_path: str | Path,
                   name_filter: str | None = None) -> int:
    """Write a readable, editable dump of every record in an archive."""
    c = SmbContainer.parse_file(path)
    recs = find_records(c)
    if name_filter:
        pat = name_filter.lower()
        recs = [r for r in recs if pat in r.name.lower()]

    lines = [
        f"# SWSE record dump - {Path(path).name}",
        f"# {len(recs)} records. Edit the numbers after '=' and re-import.",
        "# Lines starting with # are ignored. Do not change offsets.",
        "",
    ]
    schema = load_schema()
    for r in recs:
        if not r.values:
            continue
        cls = guess_class(r, schema)
        lines.append(f"[{r.name}]")
        lines.append(f"# node_id=0x{r.node_id:08X} data_off=0x{r.data_off:X} "
                     f"len={r.data_len}" + (f"  class={cls}" if cls else ""))
        for off, kind, val, fname in label_record(r, schema):
            # 9 significant digits round-trips a float32 exactly, so an
            # unedited dump re-imports byte-identically.
            v = f"{val:.9g}" if kind == "f" else str(val)
            label = f"   # {fname}" if fname else ""
            lines.append(f"0x{off:06X} {kind} = {v}{label}")
        lines.append("")
    Path(out_path).write_text("\n".join(lines), encoding="utf-8")
    return len(recs)


_EDIT_RE = re.compile(r"^\s*0x([0-9A-Fa-f]+)\s+([fi])\s*=\s*(-?[\d.eE+]+)\s*$")


def apply_edits(archive: str | Path, edits_path: str | Path,
                out_archive: str | Path | None = None) -> int:
    """Apply an edited dump back into the archive. Returns edits applied."""
    c = SmbContainer.parse_file(archive)
    toc = bytearray(c.toc)
    applied = 0
    for line in Path(edits_path).read_text(encoding="utf-8").splitlines():
        m = _EDIT_RE.match(line)
        if not m:
            continue
        off = int(m.group(1), 16)
        kind = m.group(2)
        if off + 4 > len(toc):
            continue
        if kind == "f":
            new = struct.pack("<f", float(m.group(3)))
        else:
            new = struct.pack("<I", int(float(m.group(3))) & 0xFFFFFFFF)
        if toc[off:off + 4] != new:      # only count/write genuine changes
            toc[off:off + 4] = new
            applied += 1
    c.toc = bytes(toc)
    Path(out_archive or archive).write_bytes(c.build())
    return applied


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(
        prog="python -m oddforge.records",
        description="Export/import game records (bounties, effects, stats).")
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export", help="dump records to a readable text file")
    e.add_argument("archive"); e.add_argument("out")
    e.add_argument("--filter", default=None, help="only records matching this text")
    i = sub.add_parser("import", help="apply an edited dump back")
    i.add_argument("archive"); i.add_argument("edits")
    i.add_argument("--out", default=None)
    a = ap.parse_args(argv)

    if a.cmd == "export":
        n = export_records(a.archive, a.out, a.filter)
        print(f"exported {n} records -> {a.out}")
    else:
        n = apply_edits(a.archive, a.edits, a.out)
        print(f"applied {n} edits -> {a.out or a.archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
