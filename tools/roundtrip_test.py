"""Milestone 0 test: parse + rebuild every SMB archive, demand byte-identical output."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from oddforge.container import SmbContainer, SmbError

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\data")

ok = fail = err = 0
failures = []
for p in sorted(GAME.rglob("*.smb")):
    original = p.read_bytes()
    try:
        c = SmbContainer.parse(original)
        rebuilt = c.build()
    except SmbError as e:
        err += 1
        failures.append((p.name, f"parse error: {e}"))
        continue
    if rebuilt == original:
        ok += 1
    else:
        fail += 1
        # locate first difference for debugging
        n = min(len(rebuilt), len(original))
        diff_at = next((i for i in range(n) if rebuilt[i] != original[i]), n)
        failures.append((p.name, f"differs at offset {diff_at:#x} "
                                 f"(orig {len(original)}B, rebuilt {len(rebuilt)}B)"))

print(f"byte-identical: {ok}   different: {fail}   parse errors: {err}")
for name, why in failures[:10]:
    print(f"  {name}: {why}")
