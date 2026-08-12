#!/usr/bin/env python3
"""How much does each pass change BY ITSELF, one guest frame to the next?

This is the DENOMINATOR every cross-emulator pass score needs, and its absence
is why catalogue #91 spent a session reading one number as a defect.

THE PROBLEM IT SOLVES. tools/first_divergence.py scores each pass against the
console and flags the biggest drop. But passes are not equally stable in TIME:
the shadow atlas is nearly static geometry, while a shadow mask is a function of
which dynamic casters exist this instant. Score a stable pass and a volatile
pass against the same threshold and the volatile one always looks broken. The
camera gate pairs the two renderers on VIEWPOINT; it says nothing about world
state, and the residual frame of drift it admits costs a volatile pass far more
than a stable one.

WHAT THIS MEASURES. The console against ITSELF, adjacent dumped guest frames,
per pass key. Same emulator, same code, same everything -- so whatever
correlation is lost is lost to TIME ALONE. That number is the ceiling any
cross-emulator score for that pass can reach at one frame of drift. A pass whose
self-r is 0.13 cannot be expected to cross-score 0.9, and reporting it as a
frontier is reporting the clock.

    tools/pass_volatility.py --theirs <dir> [--keys srcC2D0:f6] [--max-pairs 4]
    tools/pass_volatility.py --selftest

HOW TO READ IT, stated because a yardstick misread is worse than none:

  * self-r is an UPPER BOUND ON WHAT THE GAP COSTS, not a target. Adjacent DUMPS
    are one guest frame apart; the camera gate holds our capture much closer
    than that on viewpoint, so the true allowance sits between self-r and 1.0.
    A cross score comfortably BELOW self-r is a real difference. A cross score
    NEAR self-r is unresolved by this instrument, not a pass.
  * A HIGH self-r does not certify a pass. It says time is not the explanation
    for a low cross score; it says nothing about decode, pairing, or geometry.
  * n is the number of adjacent pairs that actually contributed. A key present
    in some frames and absent in others yields fewer pairs, and THAT ITSELF is
    volatility -- a pass that does not exist every frame is reported with its
    presence count, never silently averaged over the frames it happened to
    appear in.

WHAT IT CANNOT SEE: anything about OUR renderer. This is one side only, on
purpose -- it is the yardstick, not the measurement.
"""
import argparse
import collections
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

CONSOLE_RE = re.compile(
    r"oracle_f(\d+)_copy(\d+)_src([CD])([0-9A-Fa-f]+)_(\d+)x(\d+)_f(\d+)_e(\d+)_"
    r"([0-9A-Fa-f]+)_(\d+)\.bin")


def scan(td):
    """Group console dumps by frame, then by (key, dest) in copy order.

    THE DESTINATION IS PART OF THE PASS IDENTITY. This title resolves the
    shadow mask and the front buffer to the same surface at the same size and
    format (srcC2D0 1280x720 f6) and they are distinguishable ONLY by endian and
    destination address. A key that omits them puts both in one bucket, and an
    ordinal join across that bucket silently scores a mask against a front
    buffer the moment either side's count changes.
    """
    frames = collections.defaultdict(list)
    for f in sorted(pathlib.Path(td).glob("oracle_f*_copy*.bin")):
        m = CONSOLE_RE.match(f.name)
        if not m:
            continue
        frames[int(m.group(1))].append((
            int(m.group(2)),                                       # copy index
            (f"{m.group(3)}{m.group(4).upper()}", int(m.group(5)),
             int(m.group(6)), int(m.group(7)), int(m.group(8)),
             m.group(9).upper()),                                  # pass key
            f))
    for fr in frames:
        frames[fr].sort()
    return frames


def ordinalise(copies):
    """(key, nth occurrence of that key within this frame) -> path."""
    seen, out = collections.Counter(), {}
    for _, key, f in copies:
        out[(key, seen[key])] = f
        seen[key] += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--theirs", help="a console dump directory")
    ap.add_argument("--keys", action="append", default=[],
                    help="restrict to keys matching this substring, e.g. "
                         "srcC2D0 or f6; repeatable. Default: every key.")
    ap.add_argument("--max-pairs", type=int, default=4,
                    help="adjacent frame pairs to average per key")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.theirs:
        raise SystemExit("REFUSING: --theirs is required. NOTHING was measured.")

    import numpy as np
    from first_divergence import load_console

    frames = scan(a.theirs)
    if len(frames) < 2:
        raise SystemExit(
            f"REFUSING: {len(frames)} frame(s) of console dumps in {a.theirs}. "
            f"Self-correlation needs at least two ADJACENT frames, so NOTHING "
            f"was measured -- this is not a verdict of 'stable'.")
    fr_list = sorted(frames)
    adjacent = [(x, y) for x, y in zip(fr_list, fr_list[1:]) if y == x + 1]
    if not adjacent:
        raise SystemExit(
            f"REFUSING: the {len(fr_list)} dumped frames ({fr_list[0]}.."
            f"{fr_list[-1]}) contain no CONSECUTIVE pair, so any 'adjacent' "
            f"measurement here would span an unknown gap. NOTHING was measured.")

    print(f"CONSOLE AGAINST ITSELF, adjacent guest frames -- the TEMPORAL "
          f"yardstick per pass.")
    print(f"{a.theirs}: {len(fr_list)} frames {fr_list[0]}..{fr_list[-1]}, "
          f"{len(adjacent)} consecutive pair(s) available.\n")

    ords = {fr: ordinalise(c) for fr, c in frames.items()}
    all_keys = sorted({k for o in ords.values() for k in o},
                      key=lambda k: (k[0][0], k[0][1], k[0][3], k[1]))
    if a.keys:
        all_keys = [k for k in all_keys
                    if any(s.lower() in
                           (f"src{k[0][0]} {k[0][1]}x{k[0][2]} f{k[0][3]} "
                            f"e{k[0][4]} {k[0][5]}").lower() for s in a.keys)]
    if not all_keys:
        raise SystemExit(
            f"REFUSING: no pass key matched --keys {a.keys}. The directory "
            f"holds {len({k for o in ords.values() for k in o})} distinct "
            f"key(s). NOTHING was measured.")

    print(f"{'pass':>34} {'ord':>4} {'self-r':>8} {'n':>3} {'seen':>6}   note")
    rows = []
    for key in all_keys:
        (src, w, h, fmt, endian, dest) = key[0]
        ordn = key[1]
        label = f"src{src} {w}x{h} f{fmt} e{endian} @{dest}"
        seen = sum(1 for fr in fr_list if key in ords[fr])
        vals, undecoded = [], 0
        for x, y in adjacent:
            if len(vals) >= a.max_pairs:
                break
            fa, fb = ords[x].get(key), ords[y].get(key)
            if fa is None or fb is None:
                continue
            ia, ea = load_console(str(fa), w, h, fmt, endian, np)
            ib, eb = load_console(str(fb), w, h, fmt, endian, np)
            if ia is None or ib is None:
                undecoded += 1
                continue
            from front_buffer_percentiles import same_picture
            base, _ = same_picture(ia, ib, np)
            vals.append(base)
        note = ""
        if seen < len(fr_list):
            note = (f"PRESENT IN ONLY {seen}/{len(fr_list)} FRAMES -- this pass "
                    f"comes and goes, which is itself volatility")
        if undecoded:
            note += f" [{undecoded} pair(s) UNDECODABLE, excluded]"
        if not vals:
            print(f"{label:>34} {ordn:>4} {'--':>8} {0:>3} {seen:>6}   "
                  f"NO ADJACENT PAIR CARRIED THIS KEY -- not measured, not "
                  f"stable. {note}")
            continue
        r = sum(vals) / len(vals)
        rows.append((r, label, ordn))
        print(f"{label:>34} {ordn:>4} {r:>8.4f} {len(vals):>3} {seen:>6}   {note}")

    if not rows:
        print("\nNOTHING WAS MEASURED: every key failed to decode or appeared "
              "in no consecutive pair.", file=sys.stderr)
        return 2
    lo, hi = min(rows), max(rows)
    print(f"\nSPREAD: {lo[1]} ord {lo[2]} at {lo[0]:.4f} .. {hi[1]} ord {hi[2]} "
          f"at {hi[0]:.4f}, across {len(rows)} pass(es).")
    print("Read a cross-emulator score for a pass against ITS OWN row, never "
          "against another pass's. A single threshold over a spread this wide "
          "prices different volatilities the same.")
    return 0


def selftest():
    """Drive BOTH classes: a pass that must read stable and one that must not.

    A yardstick that reports every pass as stable is indistinguishable from one
    that is not looking, so the negative here is a synthetic pass whose content
    genuinely changes frame to frame and which MUST come back low.
    """
    import numpy as np
    from front_buffer_percentiles import same_picture
    rng = np.random.default_rng(7)
    yy, xx = np.mgrid[0:180, 0:320]

    def blobs(seed, n=10):
        g = np.random.default_rng(seed)
        img = np.full((180, 320), 0.004)
        for _ in range(n):
            cy, cx = g.uniform(0, 180), g.uniform(0, 320)
            img += g.uniform(0.2, 1.4) * np.exp(-(((yy - cy) ** 2) / 200
                                                  + ((xx - cx) ** 2) / 200))
        return np.stack([img.astype(np.float32)] * 3, axis=-1)

    static = blobs(1)
    drift = static + 0.01 * rng.standard_normal(static.shape).astype(np.float32)
    r_stable, _ = same_picture(static, drift, np)
    r_volatile, _ = same_picture(blobs(1), blobs(2), np)
    ok_s, ok_v = r_stable > 0.9, r_volatile < 0.5
    print(f"STABLE pass (same content + sensor noise): {r_stable:+.4f} > 0.90 "
          f"-> {'PASS' if ok_s else 'FAIL'}")
    print(f"VOLATILE pass (content genuinely redrawn):  {r_volatile:+.4f} < 0.50 "
          f"-> {'PASS' if ok_v else 'FAIL'}")
    print("  a yardstick that called BOTH stable would certify every pass and "
          "measure nothing; both classes are driven here so that is caught")
    print(f"selftest: {'PASS' if ok_s and ok_v else 'FAIL'}")
    return 0 if ok_s and ok_v else 1


if __name__ == "__main__":
    sys.exit(main())
