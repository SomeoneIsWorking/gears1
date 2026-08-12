#!/usr/bin/env python3
"""Is our depth buffer on the same SCALE as the console's, at a named draw?

    tools/depth_scale_check.py --dump <depth_after_diagN.npy> --theirs <dir>
    tools/depth_scale_check.py --selftest

WHY THIS EXISTS, AND IT IS THE MOST EXPENSIVE LESSON THIS PROJECT HAS LEARNED.
Our depth buffer held EXACTLY HALF the guest value for the whole of catalog #91,
and not one instrument saw it. Every pass comparison here scores by CORRELATION,
and correlation is scale-invariant: the scene depth pass scored 0.9847 against
the console throughout and was repeatedly cited -- by me, in the brief for a
five-agent investigation -- as proof that depth was correct. It was found only
when someone finally compared the two buffers' VALUES.

A scale error is not cosmetic, because the DEPTH TEST is not scale-invariant.
Under reverse-Z GEQUAL a halved buffer lets MORE fragments pass and FEWER fail,
so every zpass stencil mark over-fires and every depth-fail shadow volume
under-fires AT THE SAME TIME. One constant, two opposite symptoms, and no
correlation anywhere in the frame moves. That is why it survived so long: it
looked like two unrelated defects pulling in opposite directions.

WHY IT IS NOT tools/first_divergence.py's SCALE COLUMN. That column compares
RESOLVED surfaces, and this title's scene depth is resolved EARLY and then
overwritten by a depth-restore draw later in the frame. Measured: the depth
resolve scores 1.0019x against the console both before and after the fix -- it
was never wrong. The corruption lived in the buffer BETWEEN resolves, which the
walk cannot see by construction. This tool reads the buffer itself, at the draw
you name, via GEARS_DRAW_DEPTH_DUMP_PS.

    GEARS_DRAW_DEPTH_DUMP_PS=<ps hash>[,marked]   writes
    scratch/<dir>/depth_after_diag<N>.npy -- float32 (H, W, 2), channel 0 depth,
    channel 1 the raw stencil byte.

WHAT A NEGATIVE PRINTS, written before the positive so silence cannot pass for a
result: the ratio, its distribution, and the count of pixels compared. "Scales
agree" always arrives with the number it agreed to and the denominator it was
measured over.
"""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

# A CONVENTION error gives a SIMPLE FRACTION; an exposure or precision
# difference gives an arbitrary one. Flagging only the fractions is what keeps
# this from firing on every legitimate difference and being tuned out.
SUSPECT = (0.125, 0.25, 0.5, 2.0, 4.0, 8.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", help="a depth_after_diag<N>.npy")
    ap.add_argument("--theirs", help="a console dump directory")
    ap.add_argument("--frame", type=int, help="console frame; default the first")
    ap.add_argument("--tolerance", type=float, default=0.02,
                    help="how close to a simple fraction counts as one")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.dump or not a.theirs:
        raise SystemExit("REFUSING: --dump and --theirs are required. NOTHING "
                         "was compared.")

    import numpy as np
    from first_divergence import load_console

    dp = pathlib.Path(a.dump)
    if not dp.is_file():
        raise SystemExit(f"REFUSING: {dp} does not exist. NOTHING was compared "
                         f"-- this is not a verdict that the scales agree.")
    arr = np.load(str(dp))
    if arr.ndim != 3 or arr.shape[-1] < 1:
        raise SystemExit(f"REFUSING: {dp} has shape {arr.shape}; expected "
                         f"(H, W, 2) from GEARS_DRAW_DEPTH_DUMP_PS.")
    ours = arr[..., 0]

    td = pathlib.Path(a.theirs)
    cands = sorted(td.glob("oracle_f*_copy*_srcD000_1280x720_f23_*.bin"))
    if not cands:
        raise SystemExit(
            f"REFUSING: no console scene-depth dumps (srcD000 1280x720 f23) in "
            f"{td}, which holds {len(list(td.glob('*.bin')))} bin(s). NOTHING "
            f"was compared.")
    if a.frame is not None:
        cands = [c for c in cands if f"_f{a.frame}_" in c.name]
        if not cands:
            raise SystemExit(f"REFUSING: no console depth dump for frame "
                             f"{a.frame}. NOTHING was compared.")
    pick = cands[0]
    con, err = load_console(str(pick), 1280, 720, 23, 2, np)
    if con is None:
        raise SystemExit(f"REFUSING: the console dump did not decode: {err}. "
                         f"NOTHING was compared.")
    theirs = con[..., 0]

    n = min(ours.shape[0], theirs.shape[0])
    o, t = ours[:n], theirs[:n]
    both = (o > 1e-9) & (t > 1e-9)
    if both.sum() < 1000:
        raise SystemExit(
            f"REFUSING: only {int(both.sum())} pixel(s) are non-zero on BOTH "
            f"sides, out of {o.size}. A ratio over that many samples is noise. "
            f"NOTHING was concluded.")
    r = t[both] / o[both]
    med = float(np.median(r))
    print(f"ours   {dp.name}: {n} rows, mean {float(o.mean()):.6f}, "
          f"range {float(o.min()):.6f}..{float(o.max()):.6f}")
    print(f"theirs {pick.name}\n       {theirs.shape[0]} rows, mean "
          f"{float(t.mean()):.6f}, range {float(t.min()):.6f}.."
          f"{float(t.max()):.6f}")
    print(f"\ncompared over {int(both.sum()):,} pixel(s) non-zero on both sides")
    print(f"  ratio console/ours:  p1 {float(np.percentile(r,1)):.4f}   "
          f"p25 {float(np.percentile(r,25)):.4f}   MEDIAN {med:.6f}   "
          f"p75 {float(np.percentile(r,75)):.4f}   "
          f"p99 {float(np.percentile(r,99)):.4f}")

    hit = next((s for s in SUSPECT if abs(med - s) <= a.tolerance * s), None)
    if hit is not None:
        frac = float(((r > hit * 0.99) & (r < hit * 1.01)).mean())
        print(f"\nSCALE ERROR: the console is {hit:g}x our depth, median "
              f"{med:.6f}, with {100*frac:.2f}% of pixels within 1% of exactly "
              f"{hit:g}.")
        print("A SIMPLE FRACTION IS A CONVENTION ERROR, not a precision or "
              "exposure difference -- a depth range, a missed halving, a bit "
              "depth. Correlation CANNOT see this: a scaled buffer correlates "
              "at ~1.0 with its original. The depth TEST can, and does: under "
              "reverse-Z GEQUAL a halved buffer over-fires every zpass stencil "
              "mark and under-fires every depth-fail shadow volume at once.")
        return 1
    if abs(med - 1.0) <= a.tolerance:
        print(f"\nSCALES AGREE: median ratio {med:.6f}, within {a.tolerance:.0%} "
              f"of 1.0, over {int(both.sum()):,} pixels. Mean absolute "
              f"difference {float(np.abs(o[both]-t[both]).mean()):.6f} against "
              f"a console mean of {float(t[both].mean()):.6f} "
              f"({100*float(np.abs(o[both]-t[both]).mean()/t[both].mean()):.3f}%).")
        return 0
    print(f"\nRATIO {med:.6f} IS NOT 1.0 AND IS NOT A SIMPLE FRACTION. That is "
          f"neither a clean pass nor the convention error this tool is shaped "
          f"for -- it is a difference in the depth VALUES themselves, which is "
          f"a finding about geometry or the depth encode, not about scale.")
    return 2


def selftest():
    """Both classes: a scaled buffer must be caught, a noisy one must not."""
    import numpy as np
    rng = np.random.default_rng(11)
    base = np.abs(rng.standard_normal((300, 300)).astype(np.float32)) * 0.02 + 0.01
    ok = True
    for factor, want in ((2.0, True), (0.5, True), (1.0, False),
                         (1.31, False)):
        r = (base * factor)[base > 0] / base[base > 0]
        med = float(np.median(r))
        hit = any(abs(med - s) <= 0.02 * s for s in SUSPECT)
        good = hit == want
        ok = ok and good
        print(f"  console = {factor:>4}x ours -> median {med:.4f}, "
              f"flagged={hit} (want {want})  {'PASS' if good else 'FAIL'}")
    # A buffer with the SAME scale but real noise must not trip it, or the tool
    # fires on every honest difference and stops being read.
    noisy = base + rng.standard_normal(base.shape).astype(np.float32) * 0.002
    med = float(np.median(noisy[base > 0] / base[base > 0]))
    hit = any(abs(med - s) <= 0.02 * s for s in SUSPECT)
    ok = ok and not hit
    print(f"  same scale + noise -> median {med:.4f}, flagged={hit} (want "
          f"False)  {'PASS' if not hit else 'FAIL'}")
    print("  a checker that only ever sees the passing case cannot be "
          "distinguished from one that always passes, which is how a factor of "
          "two survived this project for a week")
    print(f"selftest: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    # EXIT CODES ARE PART OF THE INSTRUMENT. 0 scales agree, 1 a simple-fraction
    # scale error, 2 the values differ in a way that is not a scale error, and
    # 3 a REFUSAL -- nothing was compared. A refusal sharing an exit code with a
    # finding is how "the tool ran and found nothing" gets recorded as a pass.
    try:
        sys.exit(main())
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code, file=sys.stderr)
            sys.exit(3)
        raise
