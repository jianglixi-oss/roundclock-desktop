"""Generate iOS-style app icon bitmaps (RGB565, big-endian bytes) for the round clock.
Output: firmware/main/icons.bin — 5 icons of 100x100, each 20000 bytes.
The firmware embeds this via EMBED_FILES and memcpy()s it into the icon cache.
"""
import math
import struct
from pathlib import Path
from PIL import Image, ImageDraw

S = 100            # final icon size
SS = 4             # supersampling factor for anti-aliased edges
OUT = Path(__file__).resolve().parents[1] / 'firmware' / 'main' / 'icons.bin'

WHITE = (255, 255, 255, 255)

def rgb(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i+2], 16) for i in (0, 2, 4))

def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t + 0.5) for i in range(3))

def gradient(size, c1, c2):
    g = Image.new('RGB', (size, size))
    px = g.load()
    d = size - 1
    for y in range(size):
        for x in range(size):
            px[x, y] = mix(c1, c2, (x + y) / (2.0 * d))
    return g

def rounded_mask(size, inset, radius):
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle(
        [inset, inset, size - 1 - inset, size - 1 - inset], radius=radius, fill=255)
    return m

def gloss(size):
    g = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    px = g.load()
    for y in range(size):
        a = int(max(0.0, 1.0 - y / (size * 0.45)) * 78)
        if a:
            for x in range(size):
                px[x, y] = (255, 255, 255, a)
    return g

def w(frac):
    return max(1, int(frac * 100 * SS))

# ---------- glyphs (drawn at supersampled size, centered) ----------

def glyph_clock(size):
    L = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(L)
    cx = cy = size // 2
    r = int(0.30 * size)
    ink = (20, 34, 64, 255)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=WHITE)
    for i in range(12):
        a = i * math.pi / 6
        x0 = cx + (r - int(0.020 * size)) * math.sin(a)
        y0 = cy - (r - int(0.020 * size)) * math.cos(a)
        x1 = cx + (r - int(0.050 * size)) * math.sin(a)
        y1 = cy - (r - int(0.050 * size)) * math.cos(a)
        d.line([x0, y0, x1, y1], fill=ink, width=w(0.012))
    d.line([cx, cy, cx - int(0.115 * size), cy - int(0.115 * size)], fill=ink, width=w(0.032))
    d.line([cx, cy, cx + int(0.160 * size), cy - int(0.105 * size)], fill=ink, width=w(0.020))
    d.line([cx + int(0.01 * size), cy + int(0.01 * size), cx + int(0.02 * size), cy - int(0.185 * size)],
           fill=(255, 149, 0, 255), width=w(0.012))
    d.ellipse([cx - w(0.025), cy - w(0.025), cx + w(0.025), cy + w(0.025)], fill=WHITE)
    return L

def glyph_hermes(size):
    L = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(L)
    cx = size // 2
    cy = size // 2
    bw = int(0.20 * size)      # half width of H
    bh = int(0.24 * size)      # half height of H
    t = w(0.028)               # bar thickness
    d.line([cx - bw, cy - bh, cx - bw, cy + bh], fill=WHITE, width=t)
    d.line([cx + bw, cy - bh, cx + bw, cy + bh], fill=WHITE, width=t)
    d.line([cx - bw, cy, cx + bw, cy], fill=WHITE, width=t)
    # antenna arcs (top corners)
    d.arc([cx - bw - int(0.10 * size), cy - bh - int(0.14 * size),
           cx - bw + int(0.02 * size), cy - bh + int(0.06 * size)], 180, 320, fill=WHITE, width=w(0.018))
    d.arc([cx + bw - int(0.02 * size), cy - bh - int(0.14 * size),
           cx + bw + int(0.10 * size), cy - bh + int(0.06 * size)], 220, 360, fill=WHITE, width=w(0.018))
    return L

def glyph_codex(size):
    L = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(L)
    cx = size // 2
    cy = size // 2
    t = w(0.030)
    g = int(0.16 * size)
    # <
    d.line([cx - int(0.09 * size), cy - g, cx - int(0.16 * size), cy, cx - int(0.09 * size), cy + g],
           fill=WHITE, width=t, joint='curve')
    # >
    d.line([cx + int(0.09 * size), cy - g, cx + int(0.16 * size), cy, cx + int(0.09 * size), cy + g],
           fill=WHITE, width=t, joint='curve')
    # underscore
    d.line([cx - int(0.16 * size), cy + int(0.21 * size), cx + int(0.16 * size), cy + int(0.21 * size)],
           fill=WHITE, width=t)
    return L

def glyph_stocks(size):
    L = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(L)
    cx = size // 2
    cy = size // 2
    t = w(0.022)
    left = cx - int(0.27 * size)
    right = cx + int(0.27 * size)
    pts = [left, cy + int(0.14 * size), cx - int(0.13 * size), cy + int(0.02 * size),
           cx - int(0.04 * size), cy + int(0.06 * size), cx + int(0.06 * size), cy - int(0.14 * size),
           cx + int(0.15 * size), cy - int(0.05 * size), right, cy - int(0.19 * size)]
    d.line(pts, fill=WHITE, width=t, joint='curve')
    d.line([left, cy + int(0.20 * size), right, cy + int(0.20 * size)], fill=(255, 255, 255, 110), width=w(0.010))
    return L

def glyph_settings(size):
    L = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(L)
    cx = cy = size // 2
    R = int(0.24 * size)
    tr = int(0.055 * size)
    for i in range(8):
        a = i * math.pi / 4
        tx = cx + int(R * 0.92 * math.cos(a))
        ty = cy + int(R * 0.92 * math.sin(a))
        d.ellipse([tx - tr, ty - tr, tx + tr, ty + tr], fill=WHITE)
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=WHITE)
    d.ellipse([cx - int(0.085 * size), cy - int(0.085 * size),
               cx + int(0.085 * size), cy + int(0.085 * size)], fill=(0, 0, 0, 0))
    return L

# ---------- assembly ----------

def make_icon(c1, c2, glyph):
    size = S * SS
    inset = int(6 * SS)
    radius = int(23 * SS)
    mask = rounded_mask(size, inset, radius)
    icon = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    icon.paste(gradient(size, c1, c2), (0, 0), mask)
    icon.alpha_composite(glyph(size))
    gl = gloss(size)
    gl.putalpha(Image.composite(gl.getchannel('A'), Image.new('L', (size, size), 0), mask))
    icon.alpha_composite(gl)
    return icon.resize((S, S), Image.LANCZOS)

ICONS = [
    make_icon(rgb('#5BA8F0'), rgb('#1D5FD6'), glyph_clock),
    make_icon(rgb('#FFB340'), rgb('#FF5A1F'), glyph_hermes),
    make_icon(rgb('#3FD0C0'), rgb('#0E8A7A'), glyph_codex),
    make_icon(rgb('#3A4A5C'), rgb('#0E141B'), glyph_stocks),
    make_icon(rgb('#9AA0A8'), rgb('#5A6068'), glyph_settings),
]

def to_rgb565(px):
    r, g, b = px[0], px[1], px[2]
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

data = bytearray()
for ic in ICONS:
    ic = ic.convert('RGBA')
    bg = Image.new('RGB', (S, S), (0, 0, 0))  # fully transparent -> black
    ic = Image.alpha_composite(bg.convert('RGBA'), ic).convert('RGB')
    for y in range(S):
        for x in range(S):
            v = to_rgb565(ic.getpixel((x, y)))
            data += struct.pack('>H', v)   # big-endian bytes == what pixel() stores

OUT.write_bytes(bytes(data))
print('wrote', OUT, len(data), 'bytes,', len(data) // (S * S * 2), 'icons')
