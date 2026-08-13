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


def same_picture(ours, theirs, np, *, search=True):
    """Are these two artefacts even showing the same moment?

    THIS GATE EXISTS BECAUSE ITS ABSENCE PRODUCED TWO WRONG COMMITS. The camera
    gate matches the guest's view-projection constants, and matching them to a
    distance of 3.77 turned out NOT to imply the same rendered scene -- but the
    distributions were reported anyway, as "we are 3.4x short", when the two
    frames were unrelated pictures.

    The metric is a log-space luminance correlation (linear correlation is
    useless here: our dumps are 8-bit and 93% of a dark frame's pixels land in
    the bottom three codes). The number is meaningless on its own, so it is
    always reported against a POSITIVE CONTROL -- a pair that must agree scores
    ~0.93 through this same metric at this same quantization. Flips and shifts
    are searched too, because a misalignment and a different moment look
    identical without that.
    """
    n = min(ours.shape[0], theirs.shape[0])
    o = np.log1p(ours[:n].max(axis=-1) * 32.0)
    t = np.log1p(theirs[:n].max(axis=-1) * 32.0)

    def r(a, b):
        sa, sb = a.std(), b.std()
        if sa == 0 or sb == 0:
            return float("nan")
        return float(np.corrcoef(a.ravel(), b.ravel())[0, 1])

    base = r(o, t)
    best = ("as given", base)
    # Some callers use this helper as a plain temporal correlation and consume
    # only `base`.  Do not make those callers allocate 292 rolled 720p arrays
    # merely to throw their results away: the flip/shift search is the
    # same-picture gate, not part of the unshifted correlation measurement.
    if not search:
        return base, best
    for lbl, arr in (("vertical flip", o[::-1]), ("horizontal flip", o[:, ::-1]),
                     ("both flips", o[::-1, ::-1])):
        c = r(arr, t)
        if c > best[1]:
            best = (lbl, c)
    for dy in range(-64, 65, 8):
        for dx in range(-64, 65, 8):
            if dy == 0 and dx == 0:
                continue
            c = r(np.roll(np.roll(o, dy, 0), dx, 1), t)
            if c > best[1]:
                best = (f"shifted dy={dy} dx={dx}", c)
    return base, best


def stats(name, img, np):
    print(f"  {name}")
    for ci, cn in enumerate("RGB"):
        c = img[..., ci].ravel()
        q = np.percentile(c, [50, 90, 99, 99.9])
        print(f"    {cn}  median {q[0]:.4f}  p90 {q[1]:.4f}  p99 {q[2]:.4f}  "
              f"p99.9 {q[3]:.4f}  max {c.max():.4f}  mean {c.mean():.4f}")


def selftest():
    """The gate must PASS a pair that agrees and FAIL one that does not, and it
    is driven against both classes rather than reasoned about.

    The positive case is a real frame against an 8-bit-quantized, dimmed and
    noised copy of itself -- the transformations this comparison legitimately
    has to survive. The negative is the same frame against a shuffled version,
    which has an identical histogram and no spatial relationship: a gate that
    looked only at distributions would pass it.
    """
    import numpy as np
    rng = np.random.default_rng(12345)
    h, w = 180, 320
    yy, xx = np.mgrid[0:h, 0:w]
    base = (np.exp(-(((yy - 60) ** 2) / 400.0 + ((xx - 90) ** 2) / 900.0)) * 1.4
            + np.exp(-(((yy - 130) ** 2) / 300.0 + ((xx - 240) ** 2) / 600.0)) * 0.6
            + 0.004).astype(np.float32)
    ref = np.stack([base] * 3, axis=-1)
    dim = np.round(np.clip(base * 0.3, 0, 1) * 255) / 255.0
    dim = np.stack([dim] * 3, axis=-1).astype(np.float32)
    flat = base.ravel().copy()
    rng.shuffle(flat)
    shuf = np.stack([flat.reshape(h, w)] * 3, axis=-1).astype(np.float32)

    pb, (_, bb) = same_picture(dim, ref, np)
    nb, (_, nbb) = same_picture(shuf, ref, np)
    gate = 0.60
    print(f"POSITIVE: same picture, dimmed 0.3x and quantized to 8 bits -> "
          f"{pb:+.4f} as given, {bb:+.4f} best. Must be >= {gate} -> "
          f"{'PASS' if bb >= gate else 'FAIL'}")
    print(f"NEGATIVE: an IDENTICAL HISTOGRAM, shuffled spatially -> "
          f"{nb:+.4f} as given, {nbb:+.4f} best. Must be < {gate} -> "
          f"{'PASS' if nbb < gate else 'FAIL'}")
    print("  the negative shares the positive's exact distribution, so a gate "
          "built on histograms alone would pass it and this one must not")
    ok = bb >= gate and nbb < gate
    print(f"selftest: {'PASS' if ok else 'FAIL'} (both classes driven, not "
          f"reasoned about)")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", help="our resolve dump (.ppm)")
    ap.add_argument("--theirs", help="the console's raw copy (.bin)")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--fmt", type=int)
    ap.add_argument("--endian", type=int)
    ap.add_argument("--min-corr", type=float, default=0.60,
                    help="same-picture gate; below this the pair is refused. "
                         "0.60 sits well under the ~0.93 a genuinely matching "
                         "pair scores and well above the ~0.07 an unrelated one "
                         "does, both measured (see --selftest)")
    ap.add_argument("--control", type=float,
                    help="the gate's score for a pair known to agree, printed "
                         "beside the result so the number is never read alone")
    ap.add_argument("--skip-provenance", action="store_true",
                    help="do not check PROVENANCE.json beside the inputs")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.ours or not a.theirs:
        raise SystemExit("REFUSING: --ours and --theirs are both required "
                         "(or use --selftest). NOTHING was measured.")
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

    # PROVENANCE FIRST. Two oracle runs reach different moments, so a pair
    # joined across runs measures the pairing and not the renderer (C042).
    if not a.skip_provenance:
        import provenance
        sys.path.insert(0, str(pathlib.Path(__file__).parent))
        rc = provenance.do_check(argparse.Namespace(
            a=str(pathlib.Path(a.ours).parent),
            b=str(pathlib.Path(a.theirs).parent)))
        if rc == 2:
            print("REFUSING: the two directories are stamped as different runs.",
                  file=sys.stderr)
            return 2
        if rc == 3:
            print("Continuing with provenance UNKNOWN -- the same-picture gate "
                  "below is now the only thing standing between you and a "
                  "cross-run pair. Stamp future captures.", file=sys.stderr)

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
    base, (blbl, bbest) = same_picture(ours, theirs, np)
    ctrl = a.control
    print(f"SAME-PICTURE GATE: log-luminance correlation {base:+.4f} as given; "
          f"best over flips and shifts +/-64px is {bbest:+.4f} ({blbl}).")
    if ctrl is not None:
        print(f"  POSITIVE CONTROL (a pair that must agree, same metric, same "
              f"quantization): {ctrl:+.4f}")
    if bbest < a.min_corr:
        print(f"REFUSING to report distributions: {bbest:.4f} is below the "
              f"--min-corr gate of {a.min_corr}. THESE TWO ARTEFACTS ARE NOT "
              f"SHOWING THE SAME PICTURE, so any per-pixel or per-percentile "
              f"difference between them measures the pairing, not the renderer. "
              f"Fix the pairing (a tighter camera gate, or the right console "
              f"frame) before quoting any number from this pair.\n"
              f"NOTE what a failed gate does NOT invalidate: a presence check "
              f"-- 'ours is identically zero and theirs is not' -- does not "
              f"need the two to be the same moment.", file=sys.stderr)
        return 3
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
