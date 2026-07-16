#!/usr/bin/env python3
"""Generate PNG fixtures + pixel-perfect XRGB references for png_test.c.

Each accepted fixture <name>.png gets a <name>.ref:
  int32 w, int32 h (little-endian), then w*h uint32 XRGB dwords —
  RGBA/LA images composited over BG (the same math png.c must implement).
Rejected fixtures (interlaced, 16-bit) have no .ref: the decoder must
return 0 without crashing.
"""
import os, struct, zlib, random
from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "png_fixtures")
os.makedirs(OUT, exist_ok=True)
BG = (0x33, 0x66, 0x99)          # deliberately colourful: compositing bugs glow

def ref_dump(name, img):
    """Write the XRGB reference, compositing alpha over BG (integer math)."""
    rgba = img.convert("RGBA")
    w, h = rgba.size
    with open(os.path.join(OUT, name + ".ref"), "wb") as f:
        f.write(struct.pack("<ii", w, h))
        px = rgba.load()
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                # exact integer compositing: out = (a*src + (255-a)*bg + 127) // 255
                ro = (a * r + (255 - a) * BG[0] + 127) // 255
                go = (a * g + (255 - a) * BG[1] + 127) // 255
                bo = (a * b + (255 - a) * BG[2] + 127) // 255
                f.write(struct.pack("<I", (ro << 16) | (go << 8) | bo))

def save(name, img, **kw):
    img.save(os.path.join(OUT, name + ".png"), **kw)

random.seed(42)

# 1. tiny exact RGB
img = Image.new("RGB", (3, 2))
img.putdata([(255,0,0),(0,255,0),(0,0,255),(255,255,0),(17,34,51),(200,150,100)])
save("rgb_3x2", img); ref_dump("rgb_3x2", img)

# 2. RGB gradient (dynamic Huffman territory)
img = Image.new("RGB", (120, 80))
img.putdata([((x*255)//119, (y*255)//79, ((x+y)*255)//197) for y in range(80) for x in range(120)])
save("gradient_120x80", img); ref_dump("gradient_120x80", img)

# 3. RGBA with full alpha range
img = Image.new("RGBA", (64, 64))
img.putdata([((x*4) % 256, (y*4) % 256, 128, (x*y) % 256) for y in range(64) for x in range(64)])
save("rgba_64x64", img); ref_dump("rgba_64x64", img)

# 4. palette
img = Image.new("RGB", (48, 48))
img.putdata([( (x//6)*40 % 256, (y//6)*40 % 256, ((x+y)//6)*40 % 256) for y in range(48) for x in range(48)])
img = img.convert("P", palette=Image.ADAPTIVE, colors=64)
save("palette_48x48", img); ref_dump("palette_48x48", img)

# 5. grayscale
img = Image.new("L", (32, 32))
img.putdata([ (x*8 + y) % 256 for y in range(32) for x in range(32)])
save("gray_32x32", img); ref_dump("gray_32x32", img)

# 6. gray+alpha
img = Image.new("LA", (32, 32))
img.putdata([((x*8) % 256, (y*8) % 256) for y in range(32) for x in range(32)])
save("graya_32x32", img); ref_dump("graya_32x32", img)

# 7. random noise (mostly literals) + repeated tiles (backrefs)
img = Image.new("RGB", (200, 150))
data = []
tile = [(random.randrange(256), random.randrange(256), random.randrange(256)) for _ in range(40)]
for y in range(150):
    for x in range(200):
        if y < 75: data.append((random.randrange(256), random.randrange(256), random.randrange(256)))
        else:      data.append(tile[(x + y) % 40])
img.putdata(data)
save("noise_200x150", img); ref_dump("noise_200x150", img)

# 8. REJECT: Adam7 interlaced — hand-built (PIL silently ignores interlace=True)
def chunk(typ, payload):
    c = struct.pack(">I", len(payload)) + typ + payload
    return c + struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF)
ihdr_i = struct.pack(">IIBBBBB", 32, 32, 8, 2, 0, 0, 1)   # interlace = 1 (Adam7)
raw_i = b"".join(b"\x00" + bytes(96) for _ in range(32))  # content irrelevant
png_i = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr_i)
         + chunk(b"IDAT", zlib.compress(raw_i)) + chunk(b"IEND", b""))
open(os.path.join(OUT, "interlaced_32x32.png"), "wb").write(png_i)

# 9. REJECT: 16-bit depth (hand-built minimal PNG, PIL won't write I;16 RGB)
w = h = 4
ihdr = struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0)   # 16-bit RGB
raw = b""
for y in range(h):
    raw += b"\x00" + bytes(6 * w)
png16 = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
         + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
open(os.path.join(OUT, "depth16_4x4.png"), "wb").write(png16)

# 10. stored-block PNG (compression level 0 → BTYPE=00 path)
img = Image.new("RGB", (24, 24))
img.putdata([((x*11) % 256, (y*11) % 256, 77) for y in range(24) for x in range(24)])
buf_rows = b""
px = img.load()
for y in range(24):
    buf_rows += b"\x00" + bytes(v for x in range(24) for v in px[x, y])
png0 = (b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 24, 24, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(buf_rows, 0))
        + chunk(b"IEND", b""))
open(os.path.join(OUT, "stored_24x24.png"), "wb").write(png0)
ref_dump("stored_24x24", img)

print("fixtures:", sorted(os.listdir(OUT)))
