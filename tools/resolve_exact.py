#!/usr/bin/env python3
"""Find exact half-float divergence at one composed colour resolve.

    tools/resolve_exact.py <native.rgba16f> <oracle-band.bin> [oracle-band.bin ...]
    tools/resolve_exact.py --selftest

This deliberately reports the first differing pixel and component, not an
aggregate score. Oracle bands must be contiguous guest-memory destinations of
the same f32 (RGBA16F) pass; each is untiled independently before composition.
"""

import re
import sys
from pathlib import Path

from xenos_tiled import stored_rows, tiled_offset_2d, untile


NATIVE_RE = re.compile(
    r"srcC([0-9A-Fa-f]{3})_(\d+)x(\d+)_f(\d+)_"
    r"[0-9A-Fa-f]+_draw\d+"
    r"(?:_sample(\d+)x(\d+))?\.rgba16f$")
ORACLE_RE = re.compile(
    r"srcC([0-9A-Fa-f]{3})_(\d+)x(\d+)_f(\d+)_e(\d+)_"
    r"(?:b[0-9A-Fa-f]{8}_)?([0-9A-Fa-f]{8})_(\d+)\.bin$")


def parse_native(path):
    match = NATIVE_RE.search(path.name)
    if not match:
        raise ValueError(f"native filename does not describe a colour resolve: {path}")
    base, width, height, fmt, sampled_width, sampled_height = match.groups()
    return (int(base, 16), int(width), int(height), int(fmt),
            int(sampled_width or width), int(sampled_height or height))


def parse_oracle(path):
    match = ORACLE_RE.search(path.name)
    if not match:
        raise ValueError(f"oracle filename does not describe a raw resolve: {path}")
    base, width, height, fmt, endian, dest, length = match.groups()
    if path.stat().st_size != int(length):
        raise ValueError(
            f"{path}: filename says {length} bytes, file has {path.stat().st_size}")
    return (int(base, 16), int(width), int(height), int(fmt), int(endian),
            int(dest, 16))


def oracle_half_bits(paths, expected, np):
    base, width, height, fmt, sampled_width, sampled_height = expected
    bands = []
    next_dest = None
    for path in paths:
        obase, owidth, _oheight, ofmt, endian, dest = parse_oracle(path)
        if (obase, owidth, ofmt) != (base, width, fmt):
            raise ValueError(
                f"{path}: pass identity {(obase, owidth, ofmt)} does not match "
                f"native {(base, width, fmt)}")
        if endian != 1:
            raise ValueError(f"{path}: f32 exact decode requires k8in16 endian 1")
        raw = path.read_bytes()
        if next_dest is not None and dest != next_dest:
            raise ValueError(
                f"{path}: destination {dest:#x} is not the previous band's "
                f"contiguous end {next_dest:#x}")
        rows = stored_rows(len(raw), width, 8)
        pixels = untile(raw, width, rows, np, 8) if rows else None
        if pixels is None:
            raise ValueError(f"{path}: {len(raw)} bytes cannot be untiled")
        bands.append((pixels[..., 0::2].astype(np.uint16) << 8) |
                     pixels[..., 1::2].astype(np.uint16))
        next_dest = dest + len(raw)
    joined = np.concatenate(bands, axis=0)
    if joined.shape[0] < height:
        raise ValueError(
            f"oracle bands hold {joined.shape[0]} rows, need {height}")
    return joined[:sampled_height, :sampled_width]


def compare(native_path, oracle_paths, np):
    expected = parse_native(native_path)
    _, width, height, fmt, sampled_width, sampled_height = expected
    if fmt != 32:
        raise ValueError(f"native format is f{fmt}; exact RGBA16F comparison needs f32")
    native = np.fromfile(native_path, dtype="<u2")
    if native.size != sampled_width * sampled_height * 4:
        raise ValueError(
            f"{native_path}: {native.size * 2} bytes is not "
            f"{sampled_width}x{sampled_height} RGBA16F")
    native = native.reshape(sampled_height, sampled_width, 4)
    oracle = oracle_half_bits(oracle_paths, expected, np)
    different = native != oracle
    rgb = different[..., :3]
    differing_pixels = np.argwhere(rgb.any(axis=2))
    print(f"pass srcC{expected[0]:03X} {width}x{height} f{fmt}: "
          f"{len(oracle_paths)} oracle band(s), exact half-float bits")
    print(f"RGB differing components: {int(rgb.sum())} of {rgb.size}")
    print(f"RGB differing pixels: {len(differing_pixels)} of "
          f"{sampled_width * sampled_height}")
    if not len(differing_pixels):
        print("IDENTICAL: no RGB component differs")
        return False
    y, x = map(int, differing_pixels[0])
    component = int(np.flatnonzero(rgb[y, x])[0])
    print(f"FIRST DIVERGENCE: pixel ({x},{y}) component "
          f"{'RGB'[component]}; native bits {native[y, x].tolist()}, "
          f"oracle bits {oracle[y, x].tolist()}")
    return True


def selftest(np):
    work = Path("scratch/resolve_exact_selftest")
    work.mkdir(parents=True, exist_ok=True)
    for path in work.iterdir():
        if path.is_file():
            path.unlink()
    width, height, split = 32, 64, 32
    bits = np.arange(width * height * 4, dtype=np.uint16).reshape(height, width, 4)
    native = work / "resolve_00_srcC400_32x64_f32_00000000_draw0.rgba16f"
    bits.astype("<u2").tofile(native)
    oracle_paths = []
    dest = 0x10000000
    for index, (start, rows) in enumerate(((0, split), (split, height - split))):
        linear = bits[start:start + rows]
        guest = np.empty((rows, width, 8), dtype=np.uint8)
        guest[..., 0::2] = (linear >> 8).astype(np.uint8)
        guest[..., 1::2] = linear.astype(np.uint8)
        raw = bytearray(width * rows * 8)
        for y in range(rows):
            for x in range(width):
                offset = tiled_offset_2d(x, y, width, 3)
                raw[offset:offset + 8] = guest[y, x].tobytes()
        declared_height = height if index == 0 else rows
        # Exercise both the legacy name and the current one, which carries the
        # full texture base before the dumped range's destination address.
        base_field = f"b{0x10000000:08X}_" if index else ""
        path = work / (f"oracle_f0_copy{index}_srcC400_{width}x{declared_height}_"
                       f"f32_e1_{base_field}{dest:08X}_{len(raw)}.bin")
        path.write_bytes(raw)
        oracle_paths.append(path)
        dest += len(raw)
    if compare(native, oracle_paths, np):
        print("SELFTEST FAIL: identical bands differed")
        return 1
    sampled = work / "resolve_01_srcC400_32x64_f32_00000000_draw1_sample24x64.rgba16f"
    bits[:, :24].astype("<u2").tofile(sampled)
    if compare(sampled, oracle_paths, np):
        print("SELFTEST FAIL: logical-width crop included pitch padding")
        return 1
    changed = bits.copy()
    changed[40, 7, 1] ^= 1
    changed.astype("<u2").tofile(native)
    if not compare(native, oracle_paths, np):
        print("SELFTEST FAIL: changed pixel was not detected")
        return 1
    print("SELFTEST PASS: identical composition, logical-width cropping, and a one-bit"
          " difference were distinguished")
    return 0


def main(argv):
    try:
        import numpy as np
    except ImportError as error:
        print(f"REFUSING: {error}")
        return 2
    if argv[1:] == ["--selftest"]:
        return selftest(np)
    if len(argv) < 3:
        print(__doc__)
        return 2
    native, *oracle = map(Path, argv[1:])
    try:
        for path in (native, *oracle):
            if not path.is_file():
                raise ValueError(f"missing input: {path}")
        compare(native, oracle, np)
    except (OSError, ValueError) as error:
        print(f"REFUSING: {error}. Nothing was compared.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
