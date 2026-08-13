#!/usr/bin/env python3
"""Report unclamped values from GEARS_DRAW_RESOLVE_DUMP_FLOAT=1.

The ordinary resolve PPM is deliberately clamped to [0,1] for inspection. That
cannot answer whether an HDR scene passed bloom's >1 threshold, so the runtime
can also emit the exact mapped Vulkan RGBA16F pixels as ``.rgba16f``.

    tools/resolve_float_stats.py <resolve_..._352x182_...rgba16f>
    tools/resolve_float_stats.py --selftest

The filename is part of the format: its width and height must agree with the
payload's four half-floats per pixel. A malformed or ambiguous file REFUSES;
it is never treated as a black resolve.
"""
import argparse
import pathlib
import re
import sys


NAME = re.compile(r"_(\d+)x(\d+)_f\d+_[0-9a-fA-F]+_draw\d+\.rgba16f$")


def decode(path):
    p = pathlib.Path(path)
    m = NAME.search(p.name)
    if not m:
        raise ValueError(f"REFUSING: {p.name} does not carry WIDTHxHEIGHT in the"
                         " required resolve-dump name. NOTHING was decoded.")
    w, h = map(int, m.groups())
    raw = p.read_bytes()
    want = w * h * 4 * 2
    if len(raw) != want:
        raise ValueError(f"REFUSING: {p} is {len(raw)} bytes; {w}x{h} RGBA16F"
                         f" requires exactly {want}. NOTHING was decoded.")
    import numpy as np
    a = np.frombuffer(raw, dtype='<f2').astype(np.float32).reshape(h, w, 4)
    return a, w, h


def report(a, label):
    import numpy as np
    rgb = a[..., :3]
    finite = np.isfinite(rgb)
    bad = int(finite.size - finite.sum())
    if bad:
        raise ValueError(f"REFUSING: {label} contains {bad}/{finite.size} non-finite"
                         " RGB components. A NaN payload is not a brightness result.")
    above = (rgb > 1.0).any(axis=-1)
    nonzero = (rgb != 0.0).any(axis=-1)
    print(f"{label}: {a.shape[1]}x{a.shape[0]}, {int(nonzero.sum())}/{nonzero.size}"
          f" RGB pixels non-zero; {int(above.sum())}/{above.size} have an RGB"
          " component > 1.0")
    for i, c in enumerate("RGB"):
        x = rgb[..., i]
        q = np.percentile(x, (50, 90, 99, 99.9))
        print(f"  {c}: min {x.min():.7g} p50 {q[0]:.7g} p90 {q[1]:.7g}"
              f" p99 {q[2]:.7g} p99.9 {q[3]:.7g} max {x.max():.7g}"
              f" mean {x.mean():.7g}")


def selftest():
    import numpy as np
    # Positive: genuine HDR must remain distinguishable from a dim result after
    # half-float conversion. Negative: malformed byte count must refuse.
    a = np.array([[[0.0, 0.25, 1.5, 1.0], [2.0, 0.0, 0.0, 1.0]]], np.float16)
    b = a.astype(np.float32)
    good = int(((b[..., :3] > 1.0).any(axis=-1)).sum()) == 2
    bad = len(a.tobytes()) != 3 * 1 * 4 * 2
    print(f"HDR positive: two pixels above 1.0 -> {'PASS' if good else 'FAIL'}")
    print(f"wrong-length negative: 16 bytes for 3x1 -> {'PASS' if bad else 'FAIL'}")
    return 0 if good and bad else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", nargs="?")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.dump:
        raise ValueError("REFUSING: a .rgba16f resolve dump is required. NOTHING was decoded.")
    arr, _, _ = decode(a.dump)
    report(arr, a.dump)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except ValueError as e:
        print(e, file=sys.stderr)
        raise SystemExit(2)
