#!/usr/bin/env python3
"""Shared decoding helpers for captured Xbox 360 render surfaces.

This module only decodes and classifies data. It deliberately contains no
pairing, scoring, threshold, or first-divergence policy.
"""
import pathlib


def load_console(path, width, height, fmt, endian, np):
    """Decode one tiled console surface, returning ``(image, error)``."""
    from layer_compare import depth24_to_float, stored_rows, unpack_dest, untile

    raw = pathlib.Path(path).read_bytes()
    bytes_per_pixel = 8 if fmt == 32 else 4
    rows = stored_rows(len(raw), width, bytes_per_pixel)
    if rows is None:
        return None, (f"{len(raw)} bytes is not a whole number of "
                      f"{width}-wide rows")

    pixels_all = untile(raw, width, rows, np, bpp=bytes_per_pixel)
    pixels = pixels_all[:min(height, rows)]
    if fmt in (22, 23):
        channels = [pixels[..., index].astype(np.uint32) for index in range(4)]
        if endian == 2:
            channels = channels[::-1]
        words = (channels[0] | (channels[1] << 8) |
                 (channels[2] << 16) | (channels[3] << 24))
        depth = depth24_to_float(words >> 8, fmt == 23, np)
        return np.stack([depth.astype(np.float32)] * 3, axis=-1), None

    try:
        image = unpack_dest(pixels, fmt, np, endian=endian)
    except AssertionError as error:
        return None, str(error)

    nonfinite = float((~np.isfinite(image)).mean()) if image.size else 0.0
    if nonfinite > 0.01:
        padding_note = ""
        if rows > height:
            try:
                padding = unpack_dest(pixels_all[height:rows], fmt, np,
                                      endian=endian)
                padding_note = (
                    f"; the {rows - height} alignment-padding row(s) are "
                    f"{100 * float((~np.isfinite(padding)).mean()):.1f}% "
                    "non-finite and are not counted above")
            except AssertionError:
                padding_note = (f"; the {rows - height} alignment-padding "
                                "row(s) could not be unpacked")
        return None, (
            f"{100 * nonfinite:.2f}% of components in rows 0..{height - 1} "
            f"are not finite -- a decode failure, not a difference"
            f"{padding_note}")
    return np.nan_to_num(image), None


def constant_buffer(image, np):
    """Return whether the quantized image is constant and its first value."""
    values = image.max(axis=-1)
    return float(values.var()) <= 0.0, float(values.reshape(-1)[0])
