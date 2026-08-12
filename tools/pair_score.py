#!/usr/bin/env python3
"""Did this capture actually pair the two renderers? -- scored against every
console candidate, with the positive control beside it.

A paired capture is worth exactly as much as the pairing, and until now nothing
measured that. Two methods have been scored with this and both were found
wanting: joining across separate runs (C042) scored 0.07, and the content-based
frame selector (C043) peaked at 0.49 -- where our own frame against our own
resolve of it, the same metric at the same quantization, scores 0.94.

    tools/pair_score.py --ours <dir> --theirs <dir> [--gate 0.60]
    tools/pair_score.py --selftest

EVERY CANDIDATE IS SCORED AND PRINTED, with the count, so "none passed" is
distinguishable from "none were tried" -- and the SHAPE of the scores across the
window is itself the evidence that the metric works: temporal neighbours score
higher than distant frames, and the best-fitting shift grows with distance
because the camera moves. A flat or random profile means the metric is measuring
nothing and the verdict should not be believed.

Exit 0 if a candidate passes the gate, 1 if none does, 2 on a refusal.
"""
import argparse
import glob
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours")
    ap.add_argument("--theirs")
    ap.add_argument("--gate", type=float, default=0.60,
                    help="only used by --depth, which has no drift "
                         "curve of its own yet")
    ap.add_argument("--max-drift", type=float, default=3.0,
                    help="how many frames of console-measured drift "
                         "a pair may carry and still be compared "
                         "pixelwise")
    ap.add_argument("--depth", action="store_true",
                    help="score the DEPTH pass instead of the front buffer")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.ours or not a.theirs:
        raise SystemExit("REFUSING: --ours and --theirs are required. NOTHING "
                         "was scored.")

    import numpy as np
    from front_buffer_percentiles import load_ppm, load_oracle, same_picture

    od = pathlib.Path(a.ours)
    if a.depth:
        return score_depth(od, pathlib.Path(a.theirs), a.gate, np,
                           load_ppm, load_oracle, same_picture)
    # The front buffer is the pass both sides always produce, so it is the one
    # candidate guaranteed comparable. endian 0 is this title's only e0
    # destination and marks it on the console side.
    ours_fb = sorted(od.glob("resolve_*_f6_00311000_draw*.ppm"))
    if not ours_fb:
        print(f"REFUSING: no front-buffer resolve (resolve_*_f6_00311000_*.ppm) "
              f"in {od}. It holds {len(list(od.glob('*.ppm')))} ppm(s), so this "
              f"is a capture that did not reach the front-buffer resolve, not "
              f"an empty directory.", file=sys.stderr)
        return 2
    ours = load_ppm(str(ours_fb[-1]))
    print(f"ours   {ours_fb[-1].name}")

    cands = sorted(glob.glob(str(pathlib.Path(a.theirs) / "*_f6_e0_*.bin")))
    if not cands:
        print(f"REFUSING: no console front-buffer candidates (*_f6_e0_*.bin) in "
              f"{a.theirs}, which holds {len(list(pathlib.Path(a.theirs).glob('*.bin')))} "
              f"bin(s). NOTHING was scored.", file=sys.stderr)
        return 2

    ctrl = None
    fp = od / "frame.ppm"
    if fp.exists():
        _, (_, ctrl) = same_picture(load_ppm(str(fp)), ours, np)
    print(f"POSITIVE CONTROL (our frame.ppm against our own front-buffer "
          f"resolve): {ctrl:+.4f}" if ctrl is not None else
          "POSITIVE CONTROL unavailable (no frame.ppm) -- read the scores below "
          "with that missing")

    rows = []
    for f in cands:
        th = load_oracle(f, 1280, 720, 6, 0)
        base, (lbl, best) = same_picture(ours, th, np)
        m = re.search(r"_f(\d+)_copy", pathlib.Path(f).name)
        rows.append((best, m.group(1) if m else pathlib.Path(f).name, base, lbl))
    print(f"\n{len(rows)} console candidate(s) scored:")
    for best, fr, base, lbl in rows:
        print(f"  f{fr:>6}  as-given {base:+.4f}   best {best:+.4f}  ({lbl})")

    top = max(rows)
    # THE ABSOLUTE GATE IS UNREACHABLE BY CONSTRUCTION AND ITS VERDICTS WERE
    # WRONG. 0.60 was calibrated against the positive control below -- our
    # frame.ppm against our own resolve of THE SAME INSTANT, which scores ~0.94
    # because it has zero temporal gap. Two emulators can never share an
    # instant: each advances the guest by wall-clock delta, and our side
    # captures the frame AFTER the camera matches. So no cross-emulator pair can
    # ever reach that control, and captures were being failed for existing.
    #
    # The honest denominator is the CONSOLE AGAINST ITSELF. Correlate the
    # winning console frame with its own successors and read our score off that
    # curve: the answer is "this pair is equivalent to N frames of drift", in
    # units the console itself defines. Self-calibrating, no magic number.
    drift = console_drift_curve(a.theirs, top[1], ours, np, load_oracle,
                                same_picture)
    print(f"\nBEST f{top[1]} at {top[0]:+.4f}"
          + (f"; zero-drift control {ctrl:+.4f} (OUR frame against OUR OWN "
             f"resolve of it -- unreachable by any cross-emulator pair, and "
             f"quoted only to show what a perfect score looks like)"
             if ctrl is not None else ""))
    if not drift:
        print("NO DRIFT CURVE: the console dump has no successor frames, so "
              "this score cannot be priced in frames of drift and NOTHING is "
              "concluded about the pairing. Dump a window, not one frame.",
              file=sys.stderr)
        return 2
    print("\nTHE CONSOLE AGAINST ITSELF, from the winning frame -- what one "
          "frame of drift costs THIS pass:")
    # Mark the BRACKETING pair, not one row: the score falls BETWEEN two
    # frame distances and saying it "sits here" on the lower one overstates
    # the drift by up to a whole frame.
    lo = hi = None
    for i, (k, r) in enumerate(drift):
        if r <= top[0]:
            hi = i
            lo = i - 1 if i > 0 else None
            break
    for i, (k, r) in enumerate(drift):
        tag = ""
        if i == lo or (lo is None and i == hi):
            tag = "   <- our score is just below this"
        elif i == hi:
            tag = "   <- and above this"
        print(f"   +{k} frame(s): {r:+.4f}{tag}")
    equiv = equivalent_drift(top[0], drift)
    if equiv is None:
        print(f"\nOUR PAIR SCORES {top[0]:+.4f}, BETTER THAN THE CONSOLE'S OWN "
              f"ONE-FRAME SELF-CORRELATION ({drift[0][1]:+.4f}). That is as "
              f"close as this instrument can resolve: the two renderers agree "
              f"more than the console agrees with itself a frame later. PASSES.")
        return 0
    print(f"\nOUR PAIR SCORES {top[0]:+.4f}, equivalent to about {equiv:.1f} "
          f"frame(s) of drift on the console's own scale.")
    if equiv <= a.max_drift:
        print(f"PASSES ({equiv:.1f} <= {a.max_drift} frames). Quote the "
              f"CANDIDATE THAT PASSED, not the directory -- and remember the "
              f"residual is real: anything viewpoint-sensitive still carries "
              f"{equiv:.1f} frames of camera movement.")
        return 0
    print(f"FAILS: {equiv:.1f} frames of drift exceeds {a.max_drift}. The two "
          f"sides are far enough apart in TIME that a pixelwise comparison "
          f"would be measuring the gap.", file=sys.stderr)
    return 1


def console_drift_curve(theirs, frame, ours, np, load_oracle, same_picture,
                        maxk=6):
    """The winning console frame against its own successors.

    This is the only denominator that is reachable in principle: it is measured
    on the SAME pass, the SAME content and the SAME decode as the cross score,
    and it differs from it by time alone.
    """
    try:
        f0 = int(frame)
    except (TypeError, ValueError):
        return []
    td = pathlib.Path(theirs)

    def fb(n):
        c = sorted(td.glob(f"*_f{n}_copy*_f6_e0_*.bin"))
        return load_oracle(c[0], 1280, 720, 6, 0) if c else None

    base = fb(f0)
    if base is None:
        return []
    out = []
    for k in range(1, maxk + 1):
        nxt = fb(f0 + k)
        if nxt is None:
            break
        r, _ = same_picture(base, nxt, np)
        out.append((k, float(r)))
    return out


def equivalent_drift(score, curve):
    """How many frames of console drift does `score` correspond to?

    None when the score BEATS one frame of drift, which is the good case and
    must not be reported as "0 frames" -- it is off the top of the scale.
    """
    if not curve or score > curve[0][1]:
        return None
    prev_k, prev_r = curve[0]
    for k, r in curve[1:]:
        if score >= r:
            span = prev_r - r
            frac = (prev_r - score) / span if span > 1e-9 else 0.0
            return prev_k + frac * (k - prev_k)
        prev_k, prev_r = k, r
    return float(curve[-1][0])


def scene(np, rng, n_blobs, h=180, w=320):
    """A synthetic frame with structure at several scales.

    A SINGLE SMOOTH BLOB IS THE WRONG NEGATIVE and this file learned that the
    hard way: rolling one 90 px still scored 0.81, because a low-frequency image
    correlates with a shifted copy of itself. A different game moment is not a
    shifted frame, it is a DIFFERENT ARRANGEMENT OF CONTENT, so the negative
    below is a different scene rather than the same one moved.
    """
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.full((h, w), 0.004, dtype=np.float64)
    for _ in range(n_blobs):
        cy, cx = rng.uniform(0, h), rng.uniform(0, w)
        sy, sx = rng.uniform(20, 300), rng.uniform(20, 300)
        img += rng.uniform(0.2, 1.4) * np.exp(-(((yy - cy) ** 2) / sy
                                                + ((xx - cx) ** 2) / sx))
    return np.stack([img.astype(np.float32)] * 3, axis=-1)


def score_depth(od, td, gate, np, load_ppm, load_oracle, same_picture):
    """Score the DEPTH pass, which separates two failures a colour score cannot.

    Depth is a function of GEOMETRY and VIEWPOINT alone -- no lighting, no
    tonemap, no bloom, none of the things catalogue #62 is about. So:

      depth agrees, colour does not  -> the pairing is GOOD and the difference
                                        is in shading. That is a finding about
                                        the renderer, and the colour comparison
                                        can be trusted.
      neither agrees                 -> the pairing is bad: different viewpoint
                                        or different world state. Nothing about
                                        shading can be concluded.
      depth disagrees, colour agrees -> the instrument is suspect; say so rather
                                        than picking whichever suits.

    Ours is an 8-bit grey dump and the console's is packed 24:8 decoded to
    float, so the two are NOT on the same scale and only their CORRELATION is
    meaningful here -- never their difference. The scorer is scale-invariant by
    construction, which is why this works at all.
    """
    ours_d = sorted(od.glob("resolve_*_srcD000_*_f23_*.ppm"))
    if not ours_d:
        print(f"REFUSING: no srcD000 f23 depth resolve in {od}; it holds "
              f"{len(list(od.glob('*.ppm')))} ppm(s). NOTHING was scored.",
              file=sys.stderr)
        return 2
    cands = sorted(td.glob("*_srcD000_1280x720_f23_*.bin"))
    if not cands:
        print(f"REFUSING: no console srcD000 1280x720 f23 dumps in {td}. "
              f"NOTHING was scored.", file=sys.stderr)
        return 2
    ours = load_ppm(str(ours_d[-1]))
    print(f"ours   {ours_d[-1].name}  (8-bit grey; only CORRELATION is "
          f"meaningful against the console's 24:8, never a difference)")
    # DEPTH HAS ITS OWN DECODE. unpack_dest covers 6/7/25/32 and NOT 23; the
    # guest's 24:8 goes through layer_compare's vectorised port of the runtime's
    # own Depth20e4To32, which is the same function the depth resolve uses. A
    # wrong exponent bias produces something that still LOOKS like a depth
    # buffer, so this reuses that code rather than approximating it.
    from layer_compare import untile, stored_rows, depth24_to_float

    def load_depth(path):
        raw = pathlib.Path(path).read_bytes()
        rows_ = stored_rows(len(raw), 1280, 4)
        if rows_ is None:
            raise SystemExit(f"REFUSING: {path} is not a whole number of rows.")
        px = untile(raw, 1280, rows_, np, bpp=4)[:min(720, rows_)]
        b0, b1, b2, b3 = (px[..., i].astype(np.uint32) for i in range(4))
        b0, b1, b2, b3 = b3, b2, b1, b0          # endian 2 (k8in32)
        w32 = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
        d = depth24_to_float(w32 >> 8, True, np)  # f23 = k_24_8_FLOAT
        return np.stack([d.astype(np.float32)] * 3, axis=-1)

    rows = []
    for f in cands:
        th = load_depth(str(f))
        base, (lbl, best) = same_picture(ours, th, np)
        m = re.search(r"_f(\d+)_copy", f.name)
        rows.append((best, m.group(1) if m else f.name, base, lbl))
    print(f"\n{len(rows)} console depth candidate(s) scored:")
    for best, fr, base, lbl in rows:
        print(f"  f{fr:>6}  as-given {base:+.4f}   best {best:+.4f}  ({lbl})")
    top = max(rows)
    print(f"\nBEST DEPTH f{top[1]} at {top[0]:+.4f}; gate {gate}")
    if top[0] >= gate:
        print("DEPTH AGREES: the viewpoint and the geometry are paired. A "
              "colour score that fails while this passes is a RENDERING "
              "difference, not a pairing failure.")
        return 0
    print("DEPTH DOES NOT AGREE EITHER: the two sides are not looking at the "
          "same geometry, so nothing about shading can be concluded from this "
          "pair. Fix the pairing first.", file=sys.stderr)
    return 1


def selftest():
    """Both classes, driven: a pair that must pass and a pair that must fail."""
    import numpy as np
    from front_buffer_percentiles import same_picture
    gate = 0.60
    ref = scene(np, np.random.default_rng(1), 12)
    # The positive survives what the real comparison does to one side: a large
    # exposure difference and an 8-bit quantization.
    dim = np.round(np.clip(ref * 0.3, 0, 1) * 255) / 255.0
    other = scene(np, np.random.default_rng(2), 12)
    _, (_, p) = same_picture(dim.astype(np.float32), ref, np)
    _, (_, n) = same_picture(other, ref, np)
    print(f"POSITIVE (same scene, dimmed 0.3x and 8-bit quantized): {p:+.4f} "
          f">= {gate} -> {'PASS' if p >= gate else 'FAIL'}")
    print(f"NEGATIVE (a DIFFERENT scene, same generator and statistics): "
          f"{n:+.4f} < {gate} -> {'PASS' if n < gate else 'FAIL'}")
    print("  the negative is a different arrangement of content, not the same "
          "frame shifted -- a shifted low-frequency image scores 0.81 and would "
          "make this gate look broken when it is the test case that is wrong")
    ok = p >= gate and n < gate
    print(f"selftest: {'PASS' if ok else 'FAIL'} (both classes driven)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
