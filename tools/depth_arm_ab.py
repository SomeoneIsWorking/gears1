#!/usr/bin/env python3
"""Score the two depth arms against ONE console reference, pass by pass.

Driven by tools/depth_arm_ab.sh, which produces the two arms from the same
frozen camera. This only reads what that produced.

    tools/depth_arm_ab.py --pair <pair-dir> --ab <pair-dir>/ab
    tools/depth_arm_ab.py --selftest

WHAT IT REPORTS PER PASS: whether each arm's buffer is CONSTANT, and each arm's
correlation against the console. Constancy is checked and reported FIRST and
separately, because it is the failure this whole comparison was blind to: a flat
buffer has zero variance, correlation is undefined on it, numpy returns nan, and
a comparer that prints that nan in a score column reports the loudest result in
the frame as a blank. That is how "the split gives a flat mask #1" survived as
the sole reason to keep the correct model switched off, while the shared arm was
producing the same flat buffer unnoticed.

HOW TO READ THE VERDICT. Two arms differing by less than the pass's own
temporal volatility are INDISTINGUISHABLE here, and this says so rather than
ranking them. A single moment cannot establish that a depth model is right; it
can only show whether the two models differ at all on it.
"""
import argparse
import collections
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))


def our_passes(d):
    out = []
    for f in pathlib.Path(d).glob("resolve_*.ppm"):
        m = re.match(r"resolve_(\d+)_src([CD])([0-9A-Fa-f]+)_(\d+)x(\d+)_f(\d+)_"
                     r"([0-9a-f]+)_draw(\d+)\.ppm", f.name)
        if m:
            out.append((int(m.group(8)),
                        f"{m.group(2)}{m.group(3).upper()}", int(m.group(4)),
                        int(m.group(5)), int(m.group(6)), m.group(7).upper(), f))
    out.sort()
    return out


def rank(rows):
    order, ranked = {}, []
    for r in rows:
        k = (r[1], r[2], r[3], r[4])
        order.setdefault(k, [])
        if r[5] not in order[k]:
            order[k].append(r[5])
        ranked.append(r + (order[k].index(r[5]),))
    return ranked


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pair")
    ap.add_argument("--ab")
    ap.add_argument("--frame", type=int)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.pair or not a.ab:
        raise SystemExit("REFUSING: --pair and --ab are required. NOTHING was "
                         "scored.")

    import numpy as np
    from front_buffer_percentiles import load_ppm, same_picture
    from first_divergence import load_console, degenerate

    td = pathlib.Path(a.pair) / "theirs"
    frames = sorted({int(m.group(1)) for f in td.glob("oracle_f*_copy*.bin")
                     if (m := re.match(r"oracle_f(\d+)_", f.name))})
    if not frames:
        raise SystemExit(f"REFUSING: no console dumps in {td}. Both arms would "
                         f"be scored against nothing; NOTHING was scored.")
    frame = a.frame if a.frame is not None else frames[0]

    theirs = []
    for f in td.glob(f"oracle_f{frame}_copy*.bin"):
        m = re.match(r"oracle_f\d+_copy(\d+)_src([CD])([0-9A-Fa-f]+)_(\d+)x(\d+)"
                     r"_f(\d+)_e(\d+)_([0-9A-Fa-f]+)_(\d+)\.bin", f.name)
        if m:
            theirs.append((int(m.group(1)),
                           f"{m.group(2)}{m.group(3).upper()}", int(m.group(4)),
                           int(m.group(5)), int(m.group(6)), int(m.group(7)), f,
                           m.group(8).upper()))
    theirs.sort()
    torder, tranked = {}, []
    for t in theirs:
        k = (t[1], t[2], t[3], t[4])
        torder.setdefault(k, [])
        if t[7] not in torder[k]:
            torder[k].append(t[7])
        tranked.append(t + (torder[k].index(t[7]),))

    arms = {}
    for arm in ("shared", "split"):
        d = pathlib.Path(a.ab) / arm
        if not d.is_dir():
            raise SystemExit(f"REFUSING: arm directory {d} is missing, so only "
                             f"one arm exists. NOTHING was compared.")
        arms[arm] = rank(our_passes(d))
    for arm, rows in arms.items():
        if not rows:
            raise SystemExit(f"REFUSING: arm '{arm}' produced no resolves. A "
                             f"capture that resolved nothing is not a depth "
                             f"model result. NOTHING was compared.")

    print(f"console frame {frame}; arms scored against the SAME reference.")
    print(f"{'pass':>24} {'shared':>18} {'split':>18}   note")

    seen = collections.Counter()
    diffs = []
    for draw, src, w, h, fmt, dest, f, rk in arms["shared"]:
        key = (src, w, h, fmt, rk)
        n = seen[key]
        seen[key] += 1
        # the same slot in the other arm
        other = [r for r in arms["split"]
                 if (r[1], r[2], r[3], r[4], r[7]) == key]
        if n >= len(other):
            continue
        osp = other[n]
        pick = [t for t in tranked if (t[1], t[2], t[3], t[4], t[8]) == key]
        label = f"src{src} {w}x{h} f{fmt}#{rk}"
        if n >= len(pick):
            print(f"{label:>24} {'--':>18} {'--':>18}   no console counterpart")
            continue
        con, err = load_console(str(pick[n][6]), w, h, fmt, pick[n][5], np)
        if con is None:
            print(f"{label:>24} {'--':>18} {'--':>18}   UNDECODED: {err}")
            continue
        cells, vals = [], {}
        for arm, path in (("shared", f), ("split", osp[6])):
            img = load_ppm(str(path))
            flat, val = degenerate(img, np)
            if flat:
                cells.append(f"FLAT {val:.3f}")
                vals[arm] = None
                continue
            m2 = min(img.shape[0], con.shape[0])
            r, _ = same_picture(img[:m2], con[:m2], np)
            cells.append(f"{r:+.4f}")
            vals[arm] = r
        note = ""
        if vals["shared"] is None and vals["split"] is None:
            note = "BOTH arms constant -- the depth model is not what decides it"
        elif vals["shared"] is None:
            note = "only the SHARED arm is degenerate"
        elif vals["split"] is None:
            note = "only the SPLIT arm is degenerate"
        else:
            d_ = vals["split"] - vals["shared"]
            diffs.append((abs(d_), label, d_))
            note = f"split {d_:+.4f} vs shared"
        print(f"{label:>24} {cells[0]:>18} {cells[1]:>18}   {note}")

    print()
    if not diffs:
        print("NO PASS WAS SCORED ON BOTH ARMS. That is not 'the arms agree' -- "
              "it is that nothing was comparable, and the run should be "
              "repeated rather than read.", file=sys.stderr)
        return 2
    diffs.sort(reverse=True)
    big, label, signed = diffs[0]
    print(f"LARGEST DIFFERENCE: {label}, split {signed:+.4f} against shared, "
          f"over {len(diffs)} pass(es) scored on both arms.")
    print("Compare that against the pass's own temporal yardstick "
          "(tools/pass_volatility.py) before calling either arm better: a "
          "difference smaller than what one guest frame costs that pass is not "
          "a result about the depth model. And ONE MOMENT CANNOT ESTABLISH A "
          "MODEL -- at most it removes a piece of evidence against one.")
    return 0


def selftest():
    """Drive both classes: arms that must read identical, and one degenerate."""
    import numpy as np
    from front_buffer_percentiles import same_picture
    from first_divergence import degenerate
    yy, xx = np.mgrid[0:120, 0:160]
    ref = np.stack([(np.exp(-((yy - 60) ** 2 + (xx - 80) ** 2) / 900.0)
                     ).astype(np.float32)] * 3, axis=-1)
    same = ref * 0.5
    flat = np.ones_like(ref)
    r_same, _ = same_picture(same, ref, np)
    f_flat, v_flat = degenerate(flat, np)
    f_ref, _ = degenerate(ref, np)
    ok = r_same > 0.9 and f_flat and v_flat == 1.0 and not f_ref
    print(f"IDENTICAL ARMS (scaled copy vs reference): {r_same:+.4f} > 0.90 -> "
          f"{'PASS' if r_same > 0.9 else 'FAIL'}")
    print(f"DEGENERATE ARM detected as constant at {v_flat:.4f}: "
          f"{'PASS' if f_flat else 'FAIL'}  "
          f"(and a structured buffer is NOT flagged: "
          f"{'PASS' if not f_ref else 'FAIL'})")
    print("  the degenerate case is the one this comparison was blind to, so it "
          "is driven here rather than assumed")
    print(f"selftest: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
