"""THE FIRST STRANGER'S WRATH MOD: stamp SWSE onto the Region 00 loading screen.

- Decodes the English loading screen texture (entry 0, sec1 offset 0)
- Overlays text
- Re-encodes the full DXT1 mip chain in place (same sizes, byte-for-byte layout)
- Backs up the original archive, writes the modded archive into the game
"""
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from oddforge.container import SmbContainer
from oddforge.dxt import decode_dxt1, encode_mip_chain, mip_sizes

from PIL import ImageDraw, ImageFont

GAME_SMB = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath"
                r"\data\global\global_loadingscreen_region_00.smb")
BACKUP_DIR = Path(__file__).resolve().parent.parent / "backups"
SCRATCH = Path(__file__).resolve().parent.parent / "scratch"

W = H = 1024

# 1. Parse + decode current texture
original = GAME_SMB.read_bytes()
c = SmbContainer.parse(original)
img = decode_dxt1(c.section1, W, H)

# 2. Overlay our mark
draw = ImageDraw.Draw(img)
try:
    font_big = ImageFont.truetype("georgia.ttf", 72)
    font_small = ImageFont.truetype("georgia.ttf", 36)
except OSError:
    font_big = ImageFont.load_default(72)
    font_small = ImageFont.load_default(36)

text1 = "ODDFORGE"
text2 = "the first Stranger's Wrath mod"
bb1 = draw.textbbox((0, 0), text1, font=font_big)
bb2 = draw.textbbox((0, 0), text2, font=font_small)
draw.text(((W - bb1[2]) / 2, 700), text1, fill=(214, 168, 84), font=font_big)
draw.text(((W - bb2[2]) / 2, 800), text2, fill=(140, 170, 170), font=font_small)
img.save(SCRATCH / "modded_loadingscreen.png")

# 3. Re-encode full mip chain, patch section1 in place
chain = encode_mip_chain(img)
expected = sum(s for _, _, s in mip_sizes(W, H))
assert len(chain) == expected, (len(chain), expected)

sec1 = bytearray(c.section1)
sec1[0 : len(chain)] = chain
c.section1 = bytes(sec1)

rebuilt = c.build()
assert len(rebuilt) == len(original), "size changed - abort"

# 4. Backup + install
BACKUP_DIR.mkdir(exist_ok=True)
backup_path = BACKUP_DIR / GAME_SMB.name
if not backup_path.exists():
    shutil.copy2(GAME_SMB, backup_path)
    print(f"backed up original -> {backup_path}")
GAME_SMB.write_bytes(rebuilt)
print(f"installed modded archive ({len(rebuilt):,} bytes) -> {GAME_SMB}")
print("restore anytime: copy the backup back over the game file")
