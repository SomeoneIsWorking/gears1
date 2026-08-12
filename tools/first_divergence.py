#!/usr/bin/env python3
"""Walk the frame in EXECUTION ORDER and name the FIRST pass that loses
agreement with the console. Fix that one. Re-run. Repeat.

WHY THIS SHAPE. A frame is a chain: each pass consumes the last one's output. A
pass that is CORRECT inherits whatever agreement its input had; a pass that is
WRONG drops it. So the interesting quantity is not "which pass disagrees most"
-- every pass after a broken one disagrees, and the worst is usually the last --
but "where does the chain first lose ground". That is the earliest place a fix
can do any good, and everything downstream of it is unattributable until it is
fixed.

    tools/first_divergence.py --pair <dir> [--frame 790]

The pair must be one produced by tools/camera_pair.sh and it must have PASSED
tools/pair_score.py, because a pair that is not the same moment produces a
divergence profile that is measuring the pairing. This refuses without that
check rather than reporting a frontier from an unpaired capture.

WHAT IT PRINTS: every pass, in our draw order, with its correlation against the
console's counterpart and the CHANGE from the previous pass. A run where nothing
diverges says so with the count of passes compared, so "the chain is clean" is
distinguishable from "nothing was compared".

WHAT IT CANNOT SEE, stated because a frontier tool that hides its blind spots is
worse than none:

  * A pass whose output is consumed WITHOUT a resolve does not appear at all,
    and neither does anything inside a pass. This localises to a pass BOUNDARY,
    which is where the next investigation starts, not ends.
  * THE CHAIN IS NOT STRICTLY LINEAR. Passes write to different destinations, so
    the previous row in draw order is not always the current row's input. A drop
    between adjacent rows is a strong hint and not a proof of causation -- check
    what the flagged pass actually samples before believing it.
  * A correlation over a nearly-empty buffer is noise. The velocity buffer of a
    slow camera is ~99% zero on both sides and scored 0.34, which this reported
    as a confident frontier until --min-coverage was added. Sparse passes are
    now skipped and SAID to be skipped, with both coverages printed.
"""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

# Correlation below this is "no agreement at all"; the FIRST pass to fall by
# more than DROP from its predecessor is the frontier. Both are reported
# alongside every number so a reader can apply their own.
DROP = 0.15


def load_console(path, w, h, fmt, endian, np):
    from layer_compare import untile, unpack_dest, stored_rows, depth24_to_float
    raw = pathlib.Path(path).read_bytes()
    bpp = 8 if fmt == 32 else 4
    rows = stored_rows(len(raw), w, bpp)
    if rows is None:
        return None, f"{len(raw)} bytes is not a whole number of {w}-wide rows"
    px = untile(raw, w, rows, np, bpp=bpp)[:min(h, rows)]
    if fmt in (22, 23):
        b = [px[..., i].astype(np.uint32) for i in range(4)]
        if endian == 2:
            b = b[::-1]
        w32 = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)
        d = depth24_to_float(w32 >> 8, fmt == 23, np)
        return np.stack([d.astype(np.float32)] * 3, axis=-1), None
    try:
        img = unpack_dest(px, fmt, np, endian=endian)
    except AssertionError as e:
        return None, str(e)
    finite = np.isfinite(img)
    bad = finite.size - int(finite.sum())
    if bad > 0.01 * finite.size:
        return None, (f"{100.0*bad/finite.size:.1f}% of components are NOT "
                      f"FINITE -- a decode that failed, not a difference")
    return np.nan_to_num(img), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pair", required=True, help="a camera_pair.sh output dir")
    ap.add_argument("--frame", type=int,
                    help="the console frame that WON the pairing; required "
                         "because scoring against a frame that did not win "
                         "measures the pairing, not the chain")
    ap.add_argument("--drop", type=float, default=DROP)
    ap.add_argument("--min-coverage", type=float, default=0.05,
                    help="skip a pass unless BOTH sides have this fraction of "
                         "non-zero pixels; a correlation over a near-empty "
                         "buffer is noise")
    a = ap.parse_args()

    import numpy as np
    from front_buffer_percentiles import load_ppm, same_picture

    root = pathlib.Path(a.pair)
    od, td = root / "ours", root / "theirs"
    for d in (od, td):
        if not d.is_dir():
            raise SystemExit(f"REFUSING: {d} is not a directory. NOTHING was "
                             f"walked.")
    if a.frame is None:
        raise SystemExit("REFUSING: --frame is required. Run tools/pair_score.py "
                         "first and pass the frame that PASSED; walking against "
                         "an arbitrary console frame measures the pairing rather "
                         "than the chain.")

    ours = []
    for f in od.glob("resolve_*.ppm"):
        m = re.match(r"resolve_(\d+)_src([CD])([0-9A-Fa-f]+)_(\d+)x(\d+)_f(\d+)_"
                     r"([0-9a-f]+)_draw(\d+)\.ppm", f.name)
        if m:
            ours.append((int(m.group(8)), int(m.group(1)),
                         f"{m.group(2)}{m.group(3).upper()}", int(m.group(4)),
                         int(m.group(5)), int(m.group(6)), f))
    ours.sort()
    theirs = []
    for f in td.glob(f"oracle_f{a.frame}_copy*.bin"):
        m = re.match(r"oracle_f\d+_copy(\d+)_src([CD])([0-9A-Fa-f]+)_(\d+)x(\d+)"
                     r"_f(\d+)_e(\d+)_([0-9A-Fa-f]+)_(\d+)\.bin", f.name)
        if m:
            theirs.append((int(m.group(1)),
                           f"{m.group(2)}{m.group(3).upper()}", int(m.group(4)),
                           int(m.group(5)), int(m.group(6)), int(m.group(7)), f))
    theirs.sort()
    if not ours or not theirs:
        raise SystemExit(f"REFUSING: {len(ours)} of our passes and "
                         f"{len(theirs)} console passes for frame {a.frame}. "
                         f"NOTHING was walked -- this is not a clean chain.")

    print(f"pair {root}   console frame {a.frame}")
    print(f"{len(ours)} of our passes, {len(theirs)} console passes; walking in "
          f"OUR draw order.\n")
    print(f"{'draw':>6} {'pass':>26} {'r':>8} {'change':>8}   note")

    used, prev, first_drop, compared = set(), None, None, 0
    for draw, ordn, src, w, h, fmt, f in ours:
        key = (src, w, h, fmt)
        pick = None
        for t in theirs:
            if t[0] in used:
                continue
            if (t[1], t[2], t[3], t[4]) == key:
                pick = t
                break
        label = f"src{src} {w}x{h} f{fmt}"
        if pick is None:
            print(f"{draw:>6} {label:>26} {'--':>8} {'--':>8}   "
                  f"NO console counterpart (structural key absent)")
            continue
        used.add(pick[0])
        con, err = load_console(str(pick[6]), w, h, fmt, pick[5], np)
        if con is None:
            print(f"{draw:>6} {label:>26} {'--':>8} {'--':>8}   "
                  f"UNDECODED: {err}")
            continue
        mine = load_ppm(str(f))
        n = min(mine.shape[0], con.shape[0])
        # A CORRELATION OVER A NEARLY-EMPTY BUFFER IS NOISE, NOT DISAGREEMENT.
        # The velocity buffer of a slow-moving camera is ~99% zero on both
        # sides, and scoring it produced a confident "FIRST LOSS OF AGREEMENT"
        # at 0.34 that was measuring a few hundred stray pixels. Coverage is
        # reported for BOTH sides on every row so a sparse pass is visible
        # rather than silently scored.
        covO = float((mine[:n].max(axis=-1) > 1e-6).mean())
        covC = float((con[:n].max(axis=-1) > 1e-6).mean())
        if min(covO, covC) < a.min_coverage:
            print(f"{draw:>6} {label:>26} {'--':>8} {'--':>8}   "
                  f"TOO SPARSE TO SCORE: ours {100*covO:.2f}% non-zero, console "
                  f"{100*covC:.2f}% (need {100*a.min_coverage:.0f}%). A "
                  f"correlation here would be noise on a few pixels."
                  + (f" NOTE the coverages differ by {covO/max(covC,1e-9):.0f}x,"
                     f" which is a real structural difference worth its own look"
                     if max(covO, covC) > 8 * max(min(covO, covC), 1e-9) else ""))
            continue
        _, (_, r) = same_picture(mine[:n], con[:n], np)
        compared += 1
        delta = "" if prev is None else f"{r - prev:+.4f}"
        note = ""
        if prev is not None and (r - prev) <= -a.drop and first_drop is None:
            first_drop = (draw, label, prev, r)
            note = "<-- FIRST LOSS OF AGREEMENT"
        print(f"{draw:>6} {label:>26} {r:>8.4f} {delta:>8}   {note}")
        prev = r

    print()
    if compared == 0:
        print("NOTHING WAS COMPARED. Every pass was unpaired or undecodable, so "
              "this says nothing about the chain.", file=sys.stderr)
        return 2
    if first_drop is None:
        print(f"NO PASS LOSES MORE THAN {a.drop} OF AGREEMENT across the "
              f"{compared} pass(es) compared. Either the chain is clean at this "
              f"granularity, or the defect is inside a pass rather than at a "
              f"boundary, or it is in a pass with no resolve -- this tool "
              f"cannot see the last two.")
        return 0
    draw, label, before, after = first_drop
    print(f"FRONTIER: draw {draw}, {label}. Agreement falls from {before:.4f} "
          f"to {after:.4f} across it.")
    print(f"That is the earliest place a fix can do any good. Every pass after "
          f"it consumes its output, so their disagreement is unattributable "
          f"until this one is fixed -- do NOT start on a later pass.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
