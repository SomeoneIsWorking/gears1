"""Authoritative Xenos 2D tiled-address helpers for resolve diagnostics."""


def stored_rows(nbytes, width, bpp):
    """Return the whole row count represented by a byte range, or None."""
    if not width or not bpp or nbytes % (width * bpp):
        return None
    return nbytes // (width * bpp)


def tiled_offset_2d(x, y, width, log2_bpp):
    """Return a texel's Xenos 2D tiled byte offset."""
    macro_y = ((y // 32) * (width // 32)) << (log2_bpp + 7)
    micro_y = ((y & 6) << 2) << log2_bpp
    base = (macro_y + ((micro_y & ~15) << 1) + (micro_y & 15)
            + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4))
    macro_x = (x // 32) << (log2_bpp + 7)
    micro_x = (x & 7) << log2_bpp
    offset = base + macro_x + ((micro_x & ~15) << 1) + (micro_x & 15)
    return (((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63)
            + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def _tiled_offsets_2d(width, height, bpp, np):
    ys, xs = np.meshgrid(np.arange(height), np.arange(width), indexing="ij")
    y, x = ys.astype(np.int64), xs.astype(np.int64)
    log2_bpp = {4: 2, 8: 3}[bpp]
    macro_y = ((y // 32) * (width // 32)) << (log2_bpp + 7)
    micro_y = ((y & 6) << 2) << log2_bpp
    base = (macro_y + ((micro_y & ~15) << 1) + (micro_y & 15)
            + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4))
    macro_x = (x // 32) << (log2_bpp + 7)
    micro_x = (x & 7) << log2_bpp
    offsets = base + macro_x + ((micro_x & ~15) << 1) + (micro_x & 15)
    return ((((offsets & ~511) << 3) + ((offsets & 448) << 2)
             + (offsets & 63) + ((y & 16) << 7)
             + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)))


def untile_range(raw, width, height, range_offset, np, bpp=4):
    """Untile the texels available in one destination byte range.

    `range_offset` is the dumped range's byte offset from the texture base.
    Returns `(pixels, valid)`, where invalid texels are zero and `valid` says
    which complete texels were actually present in the range.
    """
    if range_offset < 0:
        raise ValueError("tiled range begins before its texture base")
    source = np.frombuffer(raw, dtype=np.uint8)
    offsets = _tiled_offsets_2d(width, height, bpp, np)
    end = range_offset + len(source)
    valid = (offsets >= range_offset) & (offsets + bpp <= end)
    pixels = np.zeros((height, width, bpp), dtype=np.uint8)
    if valid.any():
        indices = offsets[valid, None] - range_offset + np.arange(bpp)
        pixels[valid] = source[indices]
    return pixels, valid


def untile(raw, width, height, np, bpp=4):
    """Untile a complete texture prefix, or return None if any texel is absent."""
    pixels, valid = untile_range(raw, width, height, 0, np, bpp)
    return pixels if valid.all() else None
