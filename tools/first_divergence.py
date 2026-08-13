#!/usr/bin/env python3
"""Walk resolved passes and name the first output with excess disagreement.

WHY THIS SHAPE. Raw correlations of different passes are not comparable:
velocity, masks, depth and colour contain different data and change at different
rates. Each pass is therefore judged against the SAME console pass's temporal
self-correlation, interpolated at the whole pair's measured drift. The first
pass whose cross-emulator score falls materially below that expectation is the
earliest resolved neighbourhood worth instrumenting further.

    tools/first_divergence.py --pair <dir> --frame 790 \
        --yardstick --drift-frames 1.13

The pair must be one produced by tools/camera_pair.sh and it must have PASSED
tools/pair_score.py, because a pair that is not the same moment produces a
divergence profile that is measuring the pairing. This refuses without that
check rather than reporting a frontier from an unpaired capture.

WHAT IT PRINTS: every pass in our draw order, its cross score, its own
drift-matched self score and the deficit. The raw change from the previous row
is display-only. A run where nothing exceeds the deficit threshold says so with
the count, distinct from "nothing was compared".

WHAT IT CANNOT SEE, stated because a frontier tool that hides its blind spots is
worse than none:

  * A pass whose output is consumed WITHOUT a resolve does not appear at all,
    and neither does anything inside a pass. This localises to a pass BOUNDARY,
    which is where the next investigation starts, not ends.
  * THE CHAIN IS NOT STRICTLY LINEAR. A flagged resolved output is a
    neighbourhood, not proof that its final draw caused the deficit; inspect
    its producer/consumer graph before changing code.
  * A correlation over a nearly-empty buffer is noise. The velocity buffer of a
    slow camera is ~99% zero on both sides and scored 0.34, which this reported
    as a confident frontier until --min-coverage was added. Sparse passes are
    now skipped and SAID to be skipped, with both coverages printed.
"""
import argparse
import collections
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

# Correlation below this is "no agreement at all"; the FIRST pass to fall by
# more than DROP from its predecessor is the frontier. Both are reported
# alongside every number so a reader can apply their own.
DROP = 0.15


def interpolate_yardstick(points, distance):
    """Interpolate the console-self score at a fractional frame distance."""
    lo = int(math.floor(distance))
    hi = int(math.ceil(distance))
    if lo not in points or hi not in points:
        return None
    if lo == hi:
        return points[lo]
    fraction = distance - lo
    return points[lo] + fraction * (points[hi] - points[lo])


def load_console(path, w, h, fmt, endian, np):
    from layer_compare import untile, unpack_dest, stored_rows, depth24_to_float
    raw = pathlib.Path(path).read_bytes()
    bpp = 8 if fmt == 32 else 4
    rows = stored_rows(len(raw), w, bpp)
    if rows is None:
        return None, f"{len(raw)} bytes is not a whole number of {w}-wide rows"
    px_all = untile(raw, w, rows, np, bpp=bpp)
    px = px_all[:min(h, rows)]
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
    # JUDGE THE DECODE ON THE ROWS THAT CARRY IMAGE, AND PRICE THE PADDING
    # SEPARATELY. A destination is w * align(h, 32) * bpp, so the rows past the
    # declared height are guest memory NEITHER side wrote; a buffer that is
    # clean where it matters and noisy only in its padding is a different
    # situation from one that is noisy throughout, and one percentage over the
    # whole allocation cannot tell them apart. `px` is already cropped to the
    # declared height, so the padding is unpacked separately to be measured
    # rather than inferred.
    frac_real = float((~np.isfinite(img)).mean()) if img.size else 0.0
    if frac_real > 0.01:
        pad_note = ""
        if rows > h:
            try:
                padimg = unpack_dest(px_all[h:rows], fmt, np, endian=endian)
                pad_note = (f"; the {rows - h} row(s) of alignment padding past "
                            f"height {h} are "
                            f"{100*float((~np.isfinite(padimg)).mean()):.1f}% "
                            f"non-finite and are NOT counted above")
            except AssertionError:
                pad_note = (f"; the {rows - h} padding row(s) could not be "
                            f"unpacked to compare against")
        return None, (f"{100.0*frac_real:.2f}% of components in rows 0..{h-1} "
                      f"are NOT FINITE -- a decode that failed, not a "
                      f"difference{pad_note}. Sweeping the endian does NOT fix "
                      f"it (best of the four leaves 0.47%), and picking one by "
                      f"NaN count would be fitting the layout to the output")
    return np.nan_to_num(img), None


# Ratios a CONVENTION error produces. An exposure or tonemap difference gives an
# arbitrary ratio; a wrong depth range, a missed halving, a 10-bit read as 8-bit
# give a SIMPLE FRACTION. That is what makes this worth flagging separately from
# "the two sides differ in brightness", which they legitimately do.
SUSPECT_SCALES = (0.125, 0.25, 0.5, 2.0, 4.0, 8.0)


def scale_ratio(mine, con, np):
    """The median console/ours ratio, and whether it is a suspect fraction.

    WHY THIS EXISTS, AND IT IS THE MOST EXPENSIVE LESSON IN THIS FILE.
    CORRELATION IS SCALE-INVARIANT. Every pass comparison in this project scored
    by correlation, so a depth buffer holding exactly HALF the guest value
    scored 0.9847 against the console and was repeatedly cited as proof that
    depth was correct -- including in the brief that ruled depth content out for
    a five-agent investigation. It was found only when someone compared the two
    buffers' VALUES.

    A scale error is not cosmetic. Correlation does not care, but the DEPTH TEST
    does: under reverse-Z GEQUAL a halved buffer lets more fragments pass and
    fewer fail, which over-fires every zpass stencil mark and under-fires every
    depth-fail shadow volume at the same time. One constant, two opposite
    symptoms, and no correlation anywhere in the frame moves.

    THIS CHECK WOULD NOT HAVE CAUGHT THAT PARTICULAR BUG AND MUST NOT BE SOLD AS
    IF IT WOULD. Measured: the scene depth RESOLVE scores 1.0019x against the
    console both before and after the fix -- it was never wrong. The buffer went
    half-scale at a depth-restore draw LATER in the frame than the resolve, so
    the resolved artefact this tool compares had already been written correctly.
    That is this walk's documented blind spot (a resource consumed without an
    intervening resolve is invisible here), not a gap this column closes. What
    catches that class is tools/depth_scale_check.py, which compares the FLOAT
    depth dump taken after a named draw against the console's decoded depth.
    This column catches a scale error that SURVIVES to a resolve, which is a
    real and different class.

    Returns (ratio, suspect_fraction_or_None). Ratio is None when the two sides
    share too few non-zero pixels to compare.
    """
    a, b = mine.max(axis=-1), con.max(axis=-1)
    both = (a > 1e-6) & (b > 1e-6)
    if both.sum() < 1000:
        return None, None
    r = float(np.median(b[both] / a[both]))
    for s in SUSPECT_SCALES:
        if abs(r - s) <= 0.02 * s:
            return r, s
    return r, None


def degenerate(img, np):
    """Is this buffer CONSTANT? -- the failure correlation cannot report.

    A correlation coefficient is undefined when either side has zero variance,
    and numpy returns nan. That is not a low score, it is NO score, and a walk
    that prints it in the score column reports the frame's most broken pass as
    a blank and then flags an innocent pass downstream instead.

    It is not a rare edge either: it is precisely what THIS defect looks like.
    A shadow mask that shadows nothing resolves to a flat 1.0 -- 921,600 pixels
    of the same value -- and that is the single most informative buffer in the
    frame. Reported, never scored.

    SAY WHAT "CONSTANT" MEANS HERE. Our resolve dumps are 8-bit PPMs, so this
    reads constant when every pixel lands in the same 1/255 bucket, NOT when the
    float surface is literally uniform. On the case this was written for, our
    mask's shading draw did touch 1.57% of pixels -- it wrote values within
    1/255 of full, which is invisible at this depth. That is still the finding
    and not an artefact: the console's counterpart carries 33 distinct values AT
    THE SAME QUANTIZATION and puts 4.88% of the screen below 254/255, so the
    comparison is like for like and it is our side that is featureless. Quote it
    as "no shadow survives 8-bit", never as "the surface is uniform" -- the
    float surface has not been looked at, and GEARS_DRAW_SURFACE_DUMP is what
    would look.
    """
    v = img.max(axis=-1)
    return float(v.var()) <= 0.0, float(v.reshape(-1)[0])


def selftest():
    """Drive both classes of every judgement this file makes.

    A checker that only ever sees the passing case is indistinguishable from one
    that always passes, and this file has shipped two such: a correlation that
    could not report a constant buffer, and no scale check at all.
    """
    import numpy as np
    rng = np.random.default_rng(3)
    base = np.abs(rng.standard_normal((200, 200, 3)).astype(np.float32)) + 0.1

    ok = True
    # SCALE: a convention error must be caught, an arbitrary brightness
    # difference must NOT be (or the flag fires on every colour pass and stops
    # being read).
    for factor, want in ((2.0, 2.0), (0.5, 0.5), (4.0, 4.0),
                         (1.37, None), (1.0, None), (0.93, None)):
        r, suspect = scale_ratio(base, base * factor, np)
        hit = suspect is not None
        good = (suspect == want) if want else not hit
        ok = ok and good
        print(f"  console = {factor:>5}x ours -> ratio {r:.4f}, "
              f"flagged={hit} (want {'a flag at ' + str(want) if want else 'NO flag'})"
              f"  {'PASS' if good else 'FAIL'}")
    # Too few shared non-zero pixels must REFUSE rather than return a number off
    # a handful of samples.
    sparse = np.zeros_like(base); sparse[0, 0] = 1.0
    r, _ = scale_ratio(sparse, base, np)
    ok = ok and r is None
    print(f"  near-empty buffer -> ratio {r} (want None)  "
          f"{'PASS' if r is None else 'FAIL'}")
    # DEGENERATE: a constant buffer must be reported, a structured one must not.
    flat, val = degenerate(np.ones_like(base), np)
    struct, _ = degenerate(base, np)
    ok = ok and flat and val == 1.0 and not struct
    print(f"  constant buffer flagged={flat} at {val:.3f}, structured "
          f"flagged={struct}  {'PASS' if flat and not struct else 'FAIL'}")
    yard = interpolate_yardstick({1: 0.80, 2: 0.60}, 1.25)
    explained = yard is not None and (yard - 0.74) < DROP
    defect = yard is not None and (yard - 0.40) >= DROP
    good = abs(yard - 0.75) < 1e-9 and explained and defect
    ok = ok and good
    print(f"  drift interpolation -> {yard:.3f}; 0.74 explained={explained}, "
          f"0.40 defect={defect}  {'PASS' if good else 'FAIL'}")
    print(f"selftest: {'PASS' if ok else 'FAIL'} (both classes driven for "
          f"scale, sparsity, degeneracy and temporal deficit)")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--pair", help="a camera_pair.sh output dir")
    ap.add_argument("--frame", type=int,
                    help="the console frame that WON the pairing; required "
                         "because scoring against a frame that did not win "
                         "measures the pairing, not the chain")
    ap.add_argument("--drop", type=float, default=DROP)
    ap.add_argument("--yardstick", action="store_true",
                    help="also measure each pass against ITSELF at the "
                         "pair's temporal distance, and judge its cross score "
                         "against that "
                         "rather than against a single global threshold. "
                         "Slower; use it before believing any frontier.")
    ap.add_argument("--drift-frames", type=float, default=1.0,
                    help="temporal distance priced by pair_score.py; per-pass "
                         "self scores are interpolated at this distance")
    ap.add_argument("--min-coverage", type=float, default=0.05,
                    help="skip a pass unless BOTH sides have this fraction of "
                         "non-zero pixels; a correlation over a near-empty "
                         "buffer is noise")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.pair:
        raise SystemExit("REFUSING: --pair is required. NOTHING was walked.")
    if not a.yardstick:
        raise SystemExit("REFUSING: --yardstick is required. Raw correlations "
                         "of different passes cannot define a renderer loss.")
    if a.drift_frames < 1.0:
        raise SystemExit("REFUSING: --drift-frames must be >= 1.0; "
                         "pair_score reports closer pairs as above its "
                         "one-frame scale, not as a fractional distance.")

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
                         int(m.group(5)), int(m.group(6)), f,
                         m.group(7).upper()))
    ours.sort()
    theirs = []
    for f in td.glob(f"oracle_f{a.frame}_copy*.bin"):
        m = re.match(r"oracle_f\d+_copy(\d+)_src([CD])([0-9A-Fa-f]+)_(\d+)x(\d+)"
                     r"_f(\d+)_e(\d+)_([0-9A-Fa-f]+)_(\d+)\.bin", f.name)
        if m:
            theirs.append((int(m.group(1)),
                           f"{m.group(2)}{m.group(3).upper()}", int(m.group(4)),
                           int(m.group(5)), int(m.group(6)), int(m.group(7)), f,
                           m.group(8).upper()))
    theirs.sort()

    # THE DESTINATION IS PART OF THE PASS IDENTITY. This title resolves the
    # shadow mask AND the front buffer to the same surface at the same size and
    # format -- srcC2D0 1280x720 f6 -- and they are told apart only by endian
    # and destination address. Keying on (src, w, h, fmt) alone puts all four
    # copies of frame 793 in ONE bucket and joins them by ordinal, so the
    # instant either side's mask count changes (catalogue C048: the console's
    # varies 4/5 between frames) the front buffer is silently scored against a
    # shadow mask and the result is reported as a pass.
    #
    # The two sides use different guest addresses for the same pass, so the
    # address cannot be compared directly. What CAN be compared is its RANK:
    # the Nth distinct destination seen for that key, in first-appearance order.
    # Masks come before the front buffer on both sides, so mask joins to mask.
    def rank_dests(rows, key_of, dest_of):
        order, ranks = {}, []
        for r in rows:
            k = key_of(r)
            order.setdefault(k, [])
            if dest_of(r) not in order[k]:
                order[k].append(dest_of(r))
            ranks.append(order[k].index(dest_of(r)))
        return ranks

    o_rank = rank_dests(ours, lambda r: (r[2], r[3], r[4], r[5]),
                        lambda r: r[7])
    t_rank = rank_dests(theirs, lambda r: (r[1], r[2], r[3], r[4]),
                        lambda r: r[7])
    ours = [r + (k,) for r, k in zip(ours, o_rank)]
    theirs = [r + (k,) for r, k in zip(theirs, t_rank)]

    # THE PER-PASS TEMPORAL YARDSTICK. A cross-emulator score means nothing
    # until you know how much that pass changes BY ITSELF between frames. The
    # shadow atlas self-correlates at 0.97 one frame apart and a shadow mask at
    # 0.13, so judging both against one threshold reports the clock as a defect
    # on the volatile one. Load the SAME pass at the floor and ceiling of the
    # pair's measured distance and interpolate exactly as pair_score does.
    def index_by_slot(rows):
        seen, out = collections.Counter(), {}
        for t in rows:
            slot = (t[1], t[2], t[3], t[4], t[8])
            out[(slot, seen[slot])] = t
            seen[slot] += 1
        return out

    next_by_distance = {}
    if a.yardstick:
        needed = {int(math.floor(a.drift_frames)),
                  int(math.ceil(a.drift_frames))}
        for distance in sorted(needed):
            nrows = []
            for f in td.glob(f"oracle_f{a.frame + distance}_copy*.bin"):
                m = re.match(r"oracle_f\d+_copy(\d+)_src([CD])([0-9A-Fa-f]+)_"
                             r"(\d+)x(\d+)_f(\d+)_e(\d+)_([0-9A-Fa-f]+)_(\d+)\.bin",
                             f.name)
                if m:
                    nrows.append((int(m.group(1)),
                                  f"{m.group(2)}{m.group(3).upper()}",
                                  int(m.group(4)), int(m.group(5)),
                                  int(m.group(6)), int(m.group(7)), f,
                                  m.group(8).upper()))
            nrows.sort()
            if nrows:
                nr = rank_dests(nrows, lambda r: (r[1], r[2], r[3], r[4]),
                                lambda r: r[7])
                next_by_distance[distance] = index_by_slot(
                    [r + (k,) for r, k in zip(nrows, nr)])
        absent = sorted(needed - set(next_by_distance))
        if absent:
            print(f"  YARDSTICK INCOMPLETE: no console dumps at distance(s) "
                  f"{absent} needed to price {a.drift_frames:.3f} frames. "
                  "Affected rows are UNPRICED and the tool will refuse a "
                  "frontier.")
    if not ours or not theirs:
        raise SystemExit(f"REFUSING: {len(ours)} of our passes and "
                         f"{len(theirs)} console passes for frame {a.frame}. "
                         f"NOTHING was walked -- this is not a clean chain.")

    print(f"pair {root}   console frame {a.frame}")
    print(f"{len(ours)} of our passes, {len(theirs)} console passes; walking in "
          f"OUR draw order.\n")
    print(f"{'draw':>6} {'pass':>26} {'r':>8} {'change':>8}   note")

    # STRUCTURAL DIFFERENCES ARE FINDINGS, NOT NOISE TO BE JOINED AWAY. If one
    # side runs a pass a different number of times, an ordinal join pairs the
    # wrong things from that point on. Say so up front rather than reporting
    # scores computed across it.
    o_counts = collections.Counter((r[2], r[3], r[4], r[5], r[8]) for r in ours)
    t_counts = collections.Counter((r[1], r[2], r[3], r[4], r[8]) for r in theirs)
    for k in sorted(set(o_counts) | set(t_counts)):
        if o_counts[k] != t_counts[k]:
            print(f"  STRUCTURAL: src{k[0]} {k[1]}x{k[2]} f{k[3]} dest#{k[4]} "
                  f"runs {o_counts[k]}x for us and {t_counts[k]}x on the "
                  f"console. Rows for this key beyond the shorter count are "
                  f"joined by ordinal against a DIFFERENT pass; treat their "
                  f"scores as unreliable.")

    used, prev, first_drop, compared, unpriced = set(), None, None, 0, 0
    slot_seen = collections.Counter()
    for draw, ordn, src, w, h, fmt, f, odest, orank in ours:
        key = (src, w, h, fmt, orank)
        pick, slot_ord = None, slot_seen[key]
        for t in theirs:
            if t[0] in used:
                continue
            if (t[1], t[2], t[3], t[4], t[8]) == key:
                pick = t
                slot_seen[key] += 1
                break
        label = f"src{src} {w}x{h} f{fmt}#{orank}"
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
        # CROP THE ALIGNMENT PADDING BEFORE SCORING. A destination is
        # w * align(h,32) * bpp, so a 432-row copy is STORED as 448 rows with
        # the last 16 left at whatever guest memory held -- zeros. Our host
        # image holds something else there and NEITHER side wrote it, so
        # scoring it reports a difference where there is no rendering at all.
        # This tool named the shadow atlas its frontier on exactly that band:
        # 13,824 px = 864 x 16, rows 432..447 of a 448-row buffer, console 0.0
        # and ours 1.0 throughout. catalog #97 retracted the same reading in
        # 2026-08-11 and it was re-derived here because the crop was missing.
        # The rule is layer_compare's: compare over what the CONSOLE wrote.
        mc, cc = mine[:n], con[:n]
        # A CONSTANT BUFFER IS THE LOUDEST RESULT THERE IS, AND CORRELATION
        # CANNOT SAY IT. Check before scoring, on both sides, and report which
        # side is flat and at what value -- a mask stuck at 1.0 means nothing
        # was shadowed and a mask stuck at 0.0 means everything was.
        flat_o, val_o = degenerate(mc, np)
        flat_c, val_c = degenerate(cc, np)
        if flat_o and flat_c:
            agree = abs(val_o - val_c) < 1e-6
            print(f"{draw:>6} {label:>26} {'FLAT':>8} {'--':>8}   "
                  f"BOTH sides constant, ours at {val_o:.4f} and the console at "
                  f"{val_c:.4f} -- "
                  + ("identical, so this pass AGREES; correlation is undefined "
                     "on it and always will be."
                     if agree else
                     "DIFFERENT constants, which is total disagreement."))
            compared += 1
            if not agree and first_drop is None:
                first_drop = (draw, label, prev if prev is not None else 1.0,
                              None, None, "two different constants")
            prev = 1.0 if agree else 0.0
            continue
        if flat_o or flat_c:
            who = "OURS" if flat_o else "THE CONSOLE"
            val = val_o if flat_o else val_c
            print(f"{draw:>6} {label:>26} {'FLAT':>8} {'--':>8}   "
                  f"DEGENERATE: {who} is CONSTANT at {val:.4f} over all "
                  f"{mc.shape[0] * mc.shape[1]} pixels AT 8-BIT while the other "
                  f"side has real structure. Correlation is UNDEFINED here -- "
                  f"numpy returns nan -- so this pass CANNOT be scored, and "
                  f"that is the STRONGEST disagreement in the frame, not a gap "
                  f"in the measurement. A mask flat at 1.0 shadows nothing "
                  f"that survives 8 bits; the float surface is NOT examined "
                  f"here, so do not quote this as a uniform surface.")
            compared += 1
            if first_drop is None:
                first_drop = (draw, label, prev if prev is not None else 1.0,
                              None, None,
                              f"{'our' if flat_o else 'the console'} side is a "
                              f"CONSTANT {val:.4f} and cannot be correlated")
            prev = 0.0
            continue
        wrote = cc.max(axis=-1) > 1e-6
        frac = float(wrote.mean())
        r_all = same_picture(mc, cc, np)[1][1]
        if frac < 0.999:
            rows_kept = np.where(wrote.any(axis=1))[0]
            if len(rows_kept):
                lo, hi = int(rows_kept.min()), int(rows_kept.max()) + 1
                mc, cc = mc[lo:hi], cc[lo:hi]
        _, (_, r) = same_picture(mc, cc, np)
        ratio, suspect = scale_ratio(mc, cc, np)
        cov_note = ("" if frac >= 0.999 else
                    f"[console wrote {100*frac:.1f}%; scored over those rows; "
                    f"whole-buffer {r_all:+.4f}] ")
        compared += 1
        # JUDGE THE SCORE AGAINST THIS PASS'S OWN VOLATILITY at the PAIR'S
        # measured temporal distance. Raw scores of different passes cannot be
        # subtracted: they transform different data and move at different rates.
        yard_points = {}
        for distance, slots in next_by_distance.items():
            nb = slots.get((key, slot_ord))
            if nb is None:
                continue
            ni, _ = load_console(str(nb[6]), w, h, fmt, nb[5], np)
            if ni is not None:
                m2 = min(cc.shape[0], ni.shape[0])
                # Deliberately unshifted: shifting turns real motion into
                # apparent stability.
                yard_points[distance], _ = same_picture(
                    cc[:m2], ni[:m2], np, search=False)
        yard = interpolate_yardstick(yard_points, a.drift_frames)
        delta = "" if prev is None else f"{r - prev:+.4f}"
        note, ystr = "", ""
        if yard is not None:
            ystr = (f"[self@{a.drift_frames:.2f} {yard:+.4f}; "
                    f"deficit {yard-r:+.4f}] ")
            if r >= yard:
                note = "at or above its own temporal yardstick"
        else:
            unpriced += 1
            note = "UNPRICED: this pass has no complete temporal yardstick"
        flagged = yard is not None and (yard - r) >= a.drop
        if yard is not None and not flagged and r < yard:
            note = (f"within {a.drop:.2f} of its own drift-matched yardstick; "
                    "raw cross-pass drops are not comparable")
        if flagged and first_drop is None:
            first_drop = (draw, label, prev, r, yard, None)
            note = "<-- FIRST OBSERVABLE LOSS OF AGREEMENT"
        scale_note = ""
        if suspect is not None:
            scale_note = (f"** SCALE {ratio:.4f}x: the console is {suspect:g}x "
                          f"our values here, to within 2%. A simple fraction is "
                          f"a CONVENTION error (a depth range, a missed halving, "
                          f"a bit depth), NOT an exposure difference -- and "
                          f"correlation cannot see it, which is why this column "
                          f"exists. ** ")
        elif ratio is not None and (ratio > 1.25 or ratio < 0.8):
            scale_note = f"[scale {ratio:.3f}x, not a simple fraction] "
        print(f"{draw:>6} {label:>26} {r:>8.4f} {delta:>8}   "
              f"{cov_note}{ystr}{scale_note}{note}")
        prev = r

    print()
    if compared == 0:
        print("NOTHING WAS COMPARED. Every pass was unpaired or undecodable, so "
              "this says nothing about the chain.", file=sys.stderr)
        return 2
    if unpriced:
        print(f"REFUSING A FRONTIER: {unpriced} of {compared} scored pass(es) "
              "lack the complete drift-matched self curve. Raw predecessor "
              "drops are display-only and cannot replace it.", file=sys.stderr)
        return 2
    if first_drop is None:
        print(f"NO PASS FALLS MORE THAN {a.drop} BELOW ITS OWN TEMPORAL "
              f"YARDSTICK across the {compared} pass(es) compared. Either the "
              "chain is clean at this "
              f"granularity, or the defect is inside a pass rather than at a "
              f"boundary, or it is in a pass with no resolve -- this tool "
              f"cannot see the last two.")
        return 0
    draw, label, before, after, yard, kind = first_drop
    if kind is not None:
        print(f"FIRST OBSERVABLE LOSS: draw {draw}, {label} -- DEGENERATE: "
              f"{kind}. This outranks any low score in the table: a pass that "
              f"cannot be correlated at all has failed harder than one that "
              f"correlates badly.")
    else:
        print(f"FIRST OBSERVABLE LOSS: draw {draw}, {label}. "
              + (f"Its score {after:.4f} falls more than {a.drop:.2f} below "
                   f"its drift-matched temporal yardstick {yard:.4f}."
                 if yard is not None else
                 f"Agreement falls from {before:.4f} to {after:.4f}, with NO "
                 "yardstick measured -- re-run with --yardstick "
                 "before believing this, because a volatile pass loses "
                 "correlation to the clock alone."))
    print("READ THIS AS A NEIGHBOURHOOD, NOT A CULPRIT. This tool sees only "
          "RESOLVED surfaces, so depth and stencil carried between resolves are "
          "invisible edges in the write graph: a pass can be scored as the "
          "first loser while the actual defect is an unresolved resource it "
          "inherited, or an overwrite of a shared destination by a pass that "
          "scored well. It says WHERE TO CHANGE INSTRUMENTS -- to a per-draw "
          "ledger of what marks and what consumes -- not what to fix.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
