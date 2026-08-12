#!/usr/bin/env python3
"""Catalog #62's open half, measured at a MATCHED CAMERA for the first time.

#62's remaining question is a DISTRIBUTION one -- "we reach p99 0.549 where the
oracle reaches 0.784" -- and every number behind it came from two scripted walks
compared frame-index to frame-index, i.e. two different game moments (#84, #98).
The camera gate (GEARS_DRAW_FRAME_CAMERA) fixed that for geometry; this applies
it to brightness.

    tools/front_buffer_percentiles.py --ours <resolve.ppm> --theirs <copy.bin> \
        [--width 1280] [--height 720] [--fmt 6] [--endian 0]

It decodes the console's raw guest bytes with layer_compare's own untiler and
unpacker -- the same trusted path (I033) -- and reports MEDIAN, p90, p99, p99.9
and max per channel for both sides, because #62 has already been sent down two
dead ends by max (one lamp reads as "full range") and by mean (a missing top end
does not move it).

WHY NOT layer_compare ITSELF: it picks the console frame whose PASS STRUCTURE is
closest to ours. That is the right key when no camera is known, and the WRONG one
when it is -- on this data it chose console frame 873 over the frame 571 our
capture was gated to. It reports means, not percentiles, as well. This takes the
pair as named and does not choose.

A missing file is a REFUSAL. A decode whose output is more than 1% non-finite is
reported as a FAILED DECODE, never as a difference.
"""
import argparse
import pathlib
import re
import sys


def load_ppm(path):
    d = pathlib.Path(path).read_bytes()
    if not d.startswith(b"P6"):
        raise SystemExit(f"REFUSING: {path} is not a P6 PPM.")
    toks, i = [], 2
    while len(toks) < 3:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        toks.append(int(d[i:j]))
        i = j
    i += 1
    w, h, _ = toks
    import numpy as np
    a = np.frombuffer(d[i:i + w * h * 3], dtype=np.uint8).astype(np.float32) / 255.0
    return a.reshape(h, w, 3)


def load_oracle(path, width, height, fmt, endian):
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    import numpy as np
    from layer_compare import untile, unpack_dest, stored_rows

    raw = pathlib.Path(path).read_bytes()
    bpp = 8 if fmt == 32 else 4
    rows = stored_rows(len(raw), width, bpp)
    if rows is None:
        raise SystemExit(f"REFUSING: {path} is {len(raw)} bytes, which is not a "
                         f"whole number of {width}-wide rows at {bpp} bpp. "
                         f"NOTHING was decoded.")
    if rows < height:
        print(f"NOTE: the console's buffer holds {rows} rows, short of {height}"
              f" -- a predicated band. Comparing over {rows} rows.")
        height = rows
    px = untile(raw, width, rows, np, bpp=bpp)[:height]
    img = unpack_dest(px, fmt, np, endian=endian)
    finite = np.isfinite(img)
    bad = finite.size - int(finite.sum())
    if bad > 0.01 * finite.size:
        raise SystemExit(f"REFUSING: {bad}/{finite.size} decoded components are "
                         f"NOT FINITE [{100.0*bad/finite.size:.1f}%]. This is a "
                         f"DECODE THAT FAILED, not a difference.")
    return np.nan_to_num(img)


def stats(name, img, np):
    print(f"  {name}")
    for ci, cn in enumerate("RGB"):
        c = img[..., ci].ravel()
        q = np.percentile(c, [50, 90, 99, 99.9])
        print(f"    {cn}  median {q[0]:.4f}  p90 {q[1]:.4f}  p99 {q[2]:.4f}  "
              f"p99.9 {q[3]:.4f}  max {c.max():.4f}  mean {c.mean():.4f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", required=True, help="our resolve dump (.ppm)")
    ap.add_argument("--theirs", required=True, help="the console's raw copy (.bin)")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--fmt", type=int)
    ap.add_argument("--endian", type=int)
    a = ap.parse_args()
    for p in (a.ours, a.theirs):
        if not pathlib.Path(p).exists():
            raise SystemExit(f"REFUSING: {p} does not exist. NOTHING was "
                             f"measured -- this is not an empty result.")
    # The console's dump names its own geometry; trust the filename over a flag
    # so a mismatched pair is caught rather than silently reinterpreted.
    m = re.search(r"_(\d+)x(\d+)_f(\d+)_e(\d+)_", pathlib.Path(a.theirs).name)
    if m:
        fw, fh, ff, fe = (int(g) for g in m.groups())
        for flag, got, nm in ((a.width, fw, "width"), (a.height, fh, "height"),
                              (a.fmt, ff, "fmt"), (a.endian, fe, "endian")):
            if flag is not None and flag != got:
                raise SystemExit(f"REFUSING: --{nm}={flag} contradicts the "
                                 f"dump's own name, which says {got}.")
        a.width, a.height, a.fmt, a.endian = fw, fh, ff, fe
    if None in (a.width, a.height, a.fmt, a.endian):
        raise SystemExit("REFUSING: the console dump's name does not carry its "
                         "geometry and not all of --width/--height/--fmt/"
                         "--endian were given. NOTHING was decoded.")

    import numpy as np
    ours = load_ppm(a.ours)
    theirs = load_oracle(a.theirs, a.width, a.height, a.fmt, a.endian)
    print(f"OURS   {a.ours}  {ours.shape[1]}x{ours.shape[0]}")
    print(f"THEIRS {a.theirs}  {theirs.shape[1]}x{theirs.shape[0]}  "
          f"fmt {a.fmt} endian {a.endian}")
    if ours.shape[:2] != theirs.shape[:2]:
        n = min(ours.shape[0], theirs.shape[0])
        print(f"NOTE: heights differ; comparing the first {n} rows of each.")
        ours, theirs = ours[:n], theirs[:n]
    print("DISTRIBUTIONS (percentiles, because #62 has been misled twice by max "
          "and once by mean):")
    stats("ours  ", ours, np)
    stats("theirs", theirs, np)
    d = np.abs(ours - theirs)
    print(f"  mean |difference| {d.mean():.4f};  "
          f"{100.0*float((d.max(axis=-1) > 0.1).mean()):.2f}% of pixels differ "
          f"by more than 0.1 in some channel")
    return 0


if __name__ == "__main__":
    sys.exit(main())
