"""Extract the game's reflection schema from stranger.exe - exactly.

WHY THIS EXISTS
    research/REFLECTION_SCHEMA.md was built by reading field-name strings in
    order and guessing where one class ended and the next began ("grouping is
    heuristic ... treat boundaries as approximate"). That is fine for browsing
    and wrong for addressing memory: it put perception fields under a class
    called `CoverDuration` and left `m_spAIPrefs` sitting in NPCPrefs at an
    offset that actually belongs to a different object. Reading a live NPC with
    those numbers returns another character's hash, which looks like data rather
    than like a mistake.

    The executable registers every reflected field with an explicit offset, so
    the exact answer is right there. This reads it.

THE PATTERN
    Each field is one heap-allocated 12-byte descriptor, built inline:

        6a 0c                 push 0Ch                 ; sizeof(descriptor)
        e8 <rel32>            call operator new
        c7 06 <typedesc>      mov  [esi],   <type>     ; type descriptor VA
        b8 <nameVA>           mov  eax,     <name>     ; field-name string
        c7 46 08 <offset>     mov  [esi+8], <offset>   ; FIELD OFFSET

    The offset is a literal in the instruction stream. No guessing.

CLASS BOUNDARIES
    Fields are registered in runs, one run per class, and each run sits in its
    own function. Rather than infer boundaries from names, this splits on the
    gaps between consecutive registrations: within a run they are a regular
    stride apart, and between runs there is a jump. That is still a heuristic,
    but a structural one - and every field carries its own offset regardless,
    so a mis-split cannot corrupt an address the way the old grouping did.

USAGE
  python tools/reflect_dump.py                     summary
  python tools/reflect_dump.py --field m_fireRate  find one field
  python tools/reflect_dump.py --csv out.csv       every field
  python tools/reflect_dump.py --near 0x17C        what else is at an offset
"""
from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\bin\stranger.exe")


class PE:
    def __init__(self, data: bytes):
        self.d = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        self.base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
        self.secs = []
        off = pe + 24 + optsz
        for _ in range(nsec):
            name = data[off:off + 8].rstrip(b"\0").decode(errors="replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
            self.secs.append((name, vaddr, vsize, rawptr, rawsize))
            off += 40

    def va_to_off(self, va: int):
        rva = va - self.base
        for _, vaddr, vsize, rawptr, rawsize in self.secs:
            if vaddr <= rva < vaddr + vsize:
                o = rawptr + (rva - vaddr)
                return o if o < len(self.d) else None
        return None

    def off_to_va(self, fo: int):
        for _, vaddr, vsize, rawptr, rawsize in self.secs:
            if rawptr <= fo < rawptr + rawsize:
                return self.base + vaddr + (fo - rawptr)
        return None

    def cstr(self, va: int, limit=64) -> str:
        o = self.va_to_off(va)
        if o is None:
            return ""
        end = self.d.find(b"\0", o, o + limit)
        if end < 0:
            return ""
        s = self.d[o:end]
        return s.decode("ascii", "replace") if re.fullmatch(rb"[\x20-\x7e]*", s) else ""


# mov [esi], <type> ; mov eax, <name> ; ... ; mov [esi+8], <offset>
FIELD_RE = re.compile(
    rb"\xc7\x06(....)"        # mov dword [esi], typedesc
    rb"\xb8(....)"            # mov eax, name VA
    rb"[\s\S]{0,24}?"         # a few instructions
    rb"\xc7\x46\x08(....)",   # mov dword [esi+8], offset
    re.DOTALL,
)


def extract(pe: PE):
    out = []
    for m in FIELD_RE.finditer(pe.d):
        typ = struct.unpack("<I", m.group(1))[0]
        nameva = struct.unpack("<I", m.group(2))[0]
        off = struct.unpack("<I", m.group(3))[0]
        name = pe.cstr(nameva)
        if not name.startswith("m_"):
            continue
        if off > 0x8000:            # not a plausible member offset
            continue
        out.append({"name": name, "offset": off, "type": typ, "at": m.start()})
    return out


def group(fields):
    """Split the flat list into runs - one run per class.

    Code position alone is not enough: several classes are registered inside a
    single function, and splitting only on a gap merged them, producing runs
    with three different fields all claiming offset 0x4. A class registers its
    fields in INCREASING offset order, so a decrease is a class boundary. That
    single rule separates them, and it is checkable - a correct run comes out
    strictly ascending, which is exactly what NPCWeaponPrefs (0x178..0x1AC in
    4-byte steps) and the perception class (0x0..0x1C) do.
    """
    runs, cur, prev_at, prev_off = [], [], None, None
    for f in fields:
        new_run = False
        if prev_at is not None and f["at"] - prev_at > 0x200:
            new_run = True                     # different function entirely
        elif prev_off is not None and f["offset"] < prev_off:
            new_run = True                     # offsets went backwards
        if new_run and cur:
            runs.append(cur)
            cur = []
        cur.append(f)
        prev_at, prev_off = f["at"], f["offset"]
    if cur:
        runs.append(cur)
    return runs


def main() -> int:
    if not EXE.exists():
        print(f"exe not found: {EXE}")
        return 1
    pe = PE(EXE.read_bytes())
    fields = extract(pe)
    runs = group(fields)
    args = sys.argv[1:]

    # name the type descriptors by how often they appear
    types = {}
    for f in fields:
        types[f["type"]] = types.get(f["type"], 0) + 1

    if "--field" in args:
        want = args[args.index("--field") + 1].lower()
        hits = [f for f in fields if want in f["name"].lower()]
        print(f"{len(hits)} match(es) for '{want}':\n")
        for f in hits:
            ri = next(i for i, r in enumerate(runs) if f in r)
            sib = [g["name"] for g in runs[ri][:6]]
            print(f"  {f['name']:34} offset {f['offset']:#06x}  "
                  f"type {f['type']:#010x}  run {ri} ({len(runs[ri])} fields)")
            print(f"      run starts: {', '.join(sib)}")
        return 0

    if "--near" in args:
        want = int(args[args.index("--near") + 1], 0)
        hits = [f for f in fields if f["offset"] == want]
        print(f"{len(hits)} field(s) at offset {want:#x}:")
        for f in hits:
            print(f"  {f['name']:36} type {f['type']:#010x}")
        return 0

    if "--csv" in args:
        out = Path(args[args.index("--csv") + 1])
        with out.open("w", encoding="utf-8") as fh:
            fh.write("run\tname\toffset\ttype\n")
            for i, r in enumerate(runs):
                for f in r:
                    fh.write(f"{i}\t{f['name']}\t{f['offset']:#x}\t{f['type']:#x}\n")
        print(f"wrote {out}  ({len(fields)} fields in {len(runs)} runs)")
        return 0

    print(f"{len(fields)} reflected fields in {len(runs)} runs\n")
    print("largest runs (a run is one class):")
    for i, r in sorted(enumerate(runs), key=lambda x: -len(x[1]))[:14]:
        span = f"{min(f['offset'] for f in r):#x}..{max(f['offset'] for f in r):#x}"
        print(f"  run {i:3}  {len(r):4} fields  offsets {span:>16}   "
              f"{', '.join(f['name'] for f in r[:3])}")
    print("\ntype descriptors by frequency:")
    for t, c in sorted(types.items(), key=lambda x: -x[1])[:12]:
        print(f"  {t:#010x}  {c:5} field(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
