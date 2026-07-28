"""SWSE mod loader CLI.

    python tools/mods_cli.py list      show mods in load order
    python tools/mods_cli.py apply     apply all enabled mods
    python tools/mods_cli.py revert    restore vanilla archives
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from oddforge.modloader import ModLoader

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath")
loader = ModLoader(game_data=GAME / "data", mods_root=GAME / "SWSEMods")

cmd = sys.argv[1] if len(sys.argv) > 1 else "list"

if cmd == "list":
    mods = loader.discover()
    print(f"{len(mods)} mod(s) in {loader.mods_root}:")
    for i, m in enumerate(mods, 1):
        print(f"  {i}. {m.name} v{m.version} by {m.author}")
        if m.description:
            print(f"     {m.description[:90]}")
elif cmd == "apply":
    print("building texture index (first run scans all archives, then cached)...")
    summary = loader.apply()
    print(f"applied mods: {summary['mods']}")
    for rel, counts in summary["archives"].items():
        print(f"  {rel}: {counts['textures']} texture(s), {counts['values']} value(s)")
    for w in summary["warnings"]:
        print(f"  [warn] {w}")
elif cmd == "revert":
    restored = loader.revert()
    print(f"restored {len(restored)} archive(s) to vanilla:")
    for r in restored:
        print(f"  {r}")
else:
    print(__doc__)
