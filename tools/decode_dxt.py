"""Decode the first texture of an SMB as DXT1 and save as PNG - format verification test."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from oddforge.container import SmbContainer

from PIL import Image


def decode_dxt1(data: bytes, width: int, height: int) -> Image.Image:
    """Minimal DXT1 (BC1) decoder."""
    img = Image.new("RGB", (width, height))
    px = img.load()
    bw, bh = width // 4, height // 4
    off = 0
    for by in range(bh):
        for bx in range(bw):
            c0, c1, bits = struct.unpack_from("<HHI", data, off)
            off += 8
            # RGB565 -> RGB888
            def expand(c):
                r = (c >> 11) & 0x1F
                g = (c >> 5) & 0x3F
                b = c & 0x1F
                return (r * 255 // 31, g * 255 // 63, b * 255 // 31)
            p0, p1 = expand(c0), expand(c1)
            if c0 > c1:
                pal = [p0, p1,
                       tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
            else:
                pal = [p0, p1,
                       tuple((a + b) // 2 for a, b in zip(p0, p1)),
                       (0, 0, 0)]
            for py in range(4):
                for pxi in range(4):
                    idx = (bits >> (2 * (py * 4 + pxi))) & 3
                    px[bx * 4 + pxi, by * 4 + py] = pal[idx]
    return img


if __name__ == "__main__":
    smb_path = sys.argv[1]
    out_path = sys.argv[2]
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 1024
    height = int(sys.argv[4]) if len(sys.argv) > 4 else 1024
    data_offset = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0

    c = SmbContainer.parse_file(smb_path)
    print(f"{Path(smb_path).name}: sec1={len(c.section1):,}B  entries={c.entry_count}")
    tex_data = c.section1[data_offset:]
    img = decode_dxt1(tex_data, width, height)
    img.save(out_path)
    print(f"saved {out_path}")
