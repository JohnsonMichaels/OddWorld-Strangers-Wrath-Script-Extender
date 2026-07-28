"""Find installed HD textures whose original used DXT1's 1-bit alpha.

THE BUG THIS EXISTS FOR
    The engine uploads these textures as GL_COMPRESSED_RGBA_S3TC_DXT1 (0x83F1),
    i.e. DXT1 *with* 1-bit alpha. But our pipeline drops alpha in both
    directions: decode_entry returns RGB, and encode_dxt1 does convert("RGB").
    So a texture that relied on transparency comes back fully opaque, and every
    formerly-clear pixel renders as solid black. In game that looks like a black
    rug lying on the ground where a decal used to be.

HOW IT DETECTS IT
    A DXT1 block encodes two 16-bit colours. If color0 > color1 the block is
    opaque 4-colour mode. If color0 <= color1 it switches to 3-colour mode where
    index 3 means TRANSPARENT. So a texture uses alpha if any block has
    color0 <= color1 and actually uses index 3 somewhere.

    That is exact, not a guess from the filename.

USAGE
  python tools/alpha_check.py                 report only
  python tools/alpha_check.py --quarantine    move offending .oft files aside
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from oddforge.container import SmbContainer
from oddforge.toc import find_textures

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath")
HD = GAME / "SWSEMods" / "SWSE HD" / "textures"
# Manifests are DISCOVERED, not listed. A hardcoded list silently skips any
# batch exported to a new folder - which is exactly how a set of transparent
# tree textures shipped as opaque black: the check reported "0 offenders"
# while never having looked at 718 of the 981 installed files. A check that
# cannot see the files is worse than no check, because it reads as a pass.
def _find_manifests(root: Path) -> list[Path]:
    found = sorted(root.glob("*/manifest.tsv")) + sorted(root.glob("*/*/manifest.tsv"))
    return [m for m in found if "up2x" not in m.parts and "up4x" not in m.parts]

FNV_OFFSET, FNV_PRIME = 2166136261, 16777619


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


def fingerprint(mip0: bytes, w: int, h: int) -> int:
    return (fnv1a(mip0[:4096])
            ^ ((w * 73856093) & 0xFFFFFFFF)
            ^ ((h * 19349663) & 0xFFFFFFFF)) & 0xFFFFFFFF


def uses_alpha(dxt1: bytes) -> bool:
    """True if any 4x4 block is in 3-colour (transparent-capable) mode AND uses it."""
    for off in range(0, len(dxt1) - 7, 8):
        c0, c1 = struct.unpack_from("<HH", dxt1, off)
        if c0 > c1:
            continue                      # opaque 4-colour block
        bits = struct.unpack_from("<I", dxt1, off + 4)[0]
        for i in range(16):
            if (bits >> (i * 2)) & 3 == 3:
                return True               # index 3 in 3-colour mode = transparent
    return False


def main() -> int:
    quarantine = "--quarantine" in sys.argv[1:]
    installed = {p.stem.upper() for p in HD.glob("*.oft")}
    if not installed:
        print(f"no .oft files installed in {HD}")
        return 0
    print(f"{len(installed)} installed .oft file(s)")

    # fingerprint -> (name, archive) from every manifest we have
    want: dict[str, tuple[str, str]] = {}
    root = Path(__file__).resolve().parent.parent
    manifests = _find_manifests(root)
    for mf in manifests:
        for line in mf.read_text(encoding="utf-8").splitlines()[1:]:
            p = line.split("\t")
            if len(p) >= 5 and p[0].upper() in installed:
                want[p[0].upper()] = (p[1], p[4])
    print(f"scanned {len(manifests)} manifest(s): "
          + ", ".join(str(m.parent.relative_to(root)) for m in manifests))
    print(f"located {len(want)} of them in the manifests")
    missing = len(installed) - len(want)
    if missing > 0:
        # Loud, because unlocated files are UNCHECKED, not clean.
        print(f"WARNING: {missing} installed .oft have no manifest entry and"
              f" were NOT checked for alpha")

    # group by archive so each is opened once
    by_arc: dict[str, list[str]] = {}
    for fp, (name, arc) in want.items():
        by_arc.setdefault(arc, []).append(fp)

    offenders = []
    for arc, fps in sorted(by_arc.items()):
        path = GAME / arc
        if not path.exists():
            continue
        try:
            c = SmbContainer.parse_file(path)
            texs = find_textures(c)
        except Exception as e:
            print(f"  !! {arc}: {e}")
            continue
        for t in texs:
            if not t.is_dxt1:
                continue
            n = (t.width // 4) * (t.height // 4) * 8
            mip0 = c.section1[t.data_offset: t.data_offset + n]
            if len(mip0) < n:
                continue
            fp = f"{fingerprint(mip0, t.width, t.height):08X}"
            if fp not in fps:
                continue
            if uses_alpha(mip0):
                offenders.append((fp, t.name, t.width, t.height))

    print(f"\n{len(offenders)} installed texture(s) NEED 1-bit alpha and are"
          f" currently rendering opaque:")
    for fp, name, w, h in sorted(offenders, key=lambda x: x[1]):
        print(f"  {fp}  {name}  ({w}x{h})")

    if not offenders:
        print("  (none - the black artifact is something else)")
        return 0

    if quarantine:
        q = HD.parent / "textures_needs_alpha"
        q.mkdir(parents=True, exist_ok=True)
        n = 0
        for fp, *_ in offenders:
            src = HD / f"{fp}.oft"
            if src.exists():
                src.replace(q / src.name)
                n += 1
        print(f"\nmoved {n} file(s) to {q}")
        print("Those textures are back to vanilla. Restart the game.")
    else:
        print("\nRe-run with --quarantine to move these aside (reverts them to vanilla).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
