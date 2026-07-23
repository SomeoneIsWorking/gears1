#!/usr/bin/env python3
"""Decode a raw BC1/BC3 blob (as dumped by GEARS_DRAW_TEX_DUMP) to a PNG.

This exists to check the guest-texture DECODE (detiling + endian) independently
of the renderer: if these images look like game art, detiling is right and any
remaining blackness is downstream (shader/blend/RT), not the upload.
"""
import struct, sys, os
from PIL import Image

def c565(v):
    r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)

def bc1_block(b, alpha1bit=True):
    c0, c1 = struct.unpack_from('<HH', b, 0)
    bits, = struct.unpack_from('<I', b, 4)
    p0, p1 = c565(c0), c565(c1)
    if c0 > c1 or not alpha1bit:
        p2 = tuple((2 * p0[i] + p1[i]) // 3 for i in range(3))
        p3 = tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3))
        a = [255] * 4
    else:
        p2 = tuple((p0[i] + p1[i]) // 2 for i in range(3))
        p3 = (0, 0, 0)
        a = [255, 255, 255, 0]
    pal = [p0, p1, p2, p3]
    out = []
    for i in range(16):
        idx = (bits >> (2 * i)) & 3
        out.append(pal[idx] + (a[idx],))
    return out

def bc3_alpha(b):
    a0, a1 = b[0], b[1]
    bits = int.from_bytes(b[2:8], 'little')
    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]

def decode(path, w, h, fmt):
    data = open(path, 'rb').read()
    bs = 8 if fmt == 'DXT1' else 16
    bw, bh = (w + 3) // 4, (h + 3) // 4
    img = Image.new('RGBA', (bw * 4, bh * 4))
    px = img.load()
    o = 0
    for by in range(bh):
        for bx in range(bw):
            blk = data[o:o + bs]; o += bs
            if fmt == 'DXT1':
                texels = bc1_block(blk)
            else:
                al = bc3_alpha(blk[:8])
                texels = [t[:3] + (al[i],) for i, t in enumerate(bc1_block(blk[8:], False))]
            for i, t in enumerate(texels):
                px[bx * 4 + i % 4, by * 4 + i // 4] = t
    return img.crop((0, 0, w, h))

if __name__ == '__main__':
    for p in sys.argv[1:]:
        name = os.path.basename(p)
        parts = name[:-4].split('_')
        dims = parts[-2]
        fmt = 'DXT1' if 'DXT1' in name else 'DXT5'
        w, h, _ = dims.split('x')
        img = decode(p, int(w), int(h), fmt)
        out = os.path.join('scratch/screenshots/texdump', name[:-4] + '.png')
        img.save(out)
        print(out, img.size)
