"""Stat-block location and editing.

Enemy/NPC weapon configs (and similar tuning data) are positional float blocks
anchored to class-name strings ("Firearm", "Club", ...) inside archive TOCs.
There are no field labels in the file - labels here come from statdefs.json and
carry a status: "confirmed" (verified in-game) or "candidate" (best guess).

A StatBlock is a window of float slots around an anchor string. Slot numbering:
slot 0 is the first float after the anchor string (+1 pad byte, observed layout);
negative slots are the 4-byte grid positions before the anchor's length prefix.
"""
from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path

from .container import SmbContainer

STATDEFS_PATH = Path(__file__).resolve().parent / "statdefs.json"

PRE_SLOTS = 20    # floats inspected before the anchor
POST_SLOTS = 14   # floats inspected after the anchor


def load_statdefs() -> dict:
    if STATDEFS_PATH.exists():
        return json.loads(STATDEFS_PATH.read_text())
    return {"anchors": {}}


@dataclass
class StatField:
    slot: int          # relative slot number (negative = before anchor)
    toc_offset: int    # absolute offset of the float in the TOC
    value: float
    label: str         # from statdefs, or "field <slot>"
    status: str        # "confirmed" | "candidate" | "unknown"


@dataclass
class StatBlock:
    anchor: str
    anchor_offset: int  # offset of anchor string in TOC
    fields: list[StatField] = field(default_factory=list)


def _clean_float(f: float) -> bool:
    return f == 0.0 or 1e-4 < abs(f) < 1e6


def find_stat_blocks(c: SmbContainer, anchors: list[str] | None = None) -> list[StatBlock]:
    defs = load_statdefs()
    if anchors is None:
        anchors = list(defs.get("anchors", {}).keys())
    toc = c.toc
    blocks: list[StatBlock] = []
    for anchor in anchors:
        needle = anchor.encode("ascii")
        labels = defs.get("anchors", {}).get(anchor, {}).get("fields", {})
        start = 0
        while True:
            i = toc.find(needle, start)
            if i < 0:
                break
            start = i + 1
            # require the length prefix to match (avoid substring hits)
            if i >= 4:
                (ln,) = struct.unpack_from("<I", toc, i - 4)
                if ln != len(needle):
                    continue
            block = StatBlock(anchor=anchor, anchor_offset=i)
            base = i + len(needle) + 1  # slot 0 position (observed +1 pad byte)
            for slot in range(-PRE_SLOTS, POST_SLOTS):
                off = base + 4 * slot if slot >= 0 else i - 4 + 4 * slot
                if off < 0 or off + 4 > len(toc):
                    continue
                (f,) = struct.unpack_from("<f", toc, off)
                if not _clean_float(f):
                    continue
                meta = labels.get(str(slot), {})
                block.fields.append(StatField(
                    slot=slot, toc_offset=off, value=f,
                    label=meta.get("label", f"field {slot}"),
                    status=meta.get("status", "unknown")))
            if block.fields:
                blocks.append(block)
    blocks.sort(key=lambda b: b.anchor_offset)
    return blocks


def find_named_blocks(c: SmbContainer, max_blocks: int = 400) -> list[StatBlock]:
    """Treat every human-readable length-prefixed string in the TOC as a potential
    record name and expose the floats that follow it.

    This is how bounty/character records in global_prefs.smb surface ("Blisterz
    Booty" -> 200.0 capture moolah, 100.0 kill moolah, health...), without needing
    a known class-name anchor.
    """
    defs = load_statdefs()
    known_anchors = set(defs.get("anchors", {}).keys())
    toc = c.toc
    n = len(toc)
    blocks: list[StatBlock] = []
    i = 36
    while i + 4 <= n and len(blocks) < max_blocks:
        (length,) = struct.unpack_from("<I", toc, i)
        if not (4 <= length <= 40 and i + 4 + length <= n):
            i += 1
            continue
        chunk = toc[i + 4 : i + 4 + length]
        if not all(32 <= b <= 126 for b in chunk):
            i += 1
            continue
        name = chunk.decode("ascii")
        if name.startswith("\\") or name in known_anchors or name.count(" ") > 6:
            i += 4 + length
            continue
        labels = defs.get("named", {}).get(name, {})
        base = i + 4 + length + 4  # skip a 4-byte hash observed after record names
        fields = []
        nonzero = set()
        for slot in range(16):
            off = base + 4 * slot
            if off + 4 > n:
                break
            (f,) = struct.unpack_from("<f", toc, off)
            if not _clean_float(f):
                break
            meta = labels.get(str(slot), {})
            fields.append(StatField(slot=slot, toc_offset=off, value=f,
                                    label=meta.get("label", f"value {slot}"),
                                    status=meta.get("status", "unknown")))
            if f != 0.0:
                nonzero.add(round(f, 6))
        if len(fields) >= 2 and len(nonzero) >= 1:
            blocks.append(StatBlock(anchor=name, anchor_offset=i, fields=fields))
            i = base + 4 * len(fields)
        else:
            i += 4 + length
    return blocks


def find_float_runs(c: SmbContainer, min_run: int = 8, max_blocks: int = 300) -> list[StatBlock]:
    """Find dense runs of plausible float values in the TOC (no anchor needed).

    Used for archives without named stat anchors (e.g. global_player.smb):
    every run becomes an editable block so unknown values can be identified
    experimentally.
    """
    toc = c.toc
    n = len(toc)
    blocks: list[StatBlock] = []
    covered_until = 0
    i = 36  # skip container fields
    while i + 4 <= n and len(blocks) < max_blocks:
        if i < covered_until:
            i += 1
            continue
        # try to grow a run at stride 4 from position i
        run: list[float] = []
        j = i
        while j + 4 <= n:
            (f,) = struct.unpack_from("<f", toc, j)
            if _clean_float(f):
                run.append(f)
                j += 4
            else:
                break
        distinct_nonzero = {round(v, 6) for v in run if v != 0.0}
        if len(run) >= min_run and len(distinct_nonzero) >= 3:
            block = StatBlock(anchor="floats", anchor_offset=i)
            for slot, v in enumerate(run[:32]):
                block.fields.append(StatField(
                    slot=slot, toc_offset=i + 4 * slot, value=v,
                    label=f"value {slot}", status="unknown"))
            blocks.append(block)
            covered_until = j
            i = j
        else:
            i += 1
    return blocks


def set_field(c: SmbContainer, f: StatField, value: float) -> None:
    toc = bytearray(c.toc)
    struct.pack_into("<f", toc, f.toc_offset, value)
    c.toc = bytes(toc)
    f.value = value
