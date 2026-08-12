#!/usr/bin/env python3
"""Where is the content in this frame -- measured so a THRESHOLD cannot invent
an answer.  Also: are two frames the same picture?

WHY THIS EXISTS.  Catalog #86 was filed, falsified, re-filed and pursued for six
rounds on the strength of a luminance-threshold bounding box.  A single
threshold over a dark scene traces the LIT SUBJECT, not the RENDERED extent, so
the same picture reads as "inset" at 0.02 and "full width" at 0.002 -- and the
issue's whole case was built from boxes taken at different thresholds on images
of different brightness.  A bounding box at one threshold is not a measurement
of geometry and this tool refuses to report one.

    tools/img_extent.py sweep <img>...          box at six thresholds + profile
    tools/img_extent.py compare <a> <b>         same picture?  crop?  scale?
    tools/img_extent.py --selftest <img>        both classes, on a real frame

READ THE SWEEP THIS WAY: a box that MOVES as the threshold falls is a bright
subject inside a darker rendered frame.  A box that HOLDS is real geometry --
content with nothing outside it at any level.  Only the second is an inset.

PPM (P6, maxval 255) and anything PIL opens.
"""
import argparse
import pathlib
import sys

THRESHOLDS = (0.002, 0.005, 0.01, 0.02, 0.05, 0.10)


def read_ppm(path, d):
    if not d.startswith(b"P6"):
        return None
    toks, i = [], 2
    while len(toks) < 3:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while i < len(d) and d[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        toks.append(int(d[i:j]))
        i = j
    i += 1
    w, h, mx = toks
    if mx != 255:
        raise SystemExit(f"REFUSING: {path} maxval {mx}; only 255 is handled, "
                         f"so NOTHING was measured.")
    px = d[i:i + w * h * 3]
    if len(px) != w * h * 3:
        raise SystemExit(f"REFUSING: {path} is truncated -- {len(px)} bytes for "
                         f"{w}x{h}. NOTHING was measured.")
    return w, h, px


def load(path):
    p = pathlib.Path(path)
    if not p.exists():
        raise SystemExit(f"REFUSING: {path} does not exist. NOTHING was "
                         f"measured -- this is not an empty result.")
    d = p.read_bytes()
    got = read_ppm(path, d)
    if got:
        return got
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit(f"REFUSING: {path} is not a P6 PPM and PIL is absent, "
                         f"so NOTHING was measured.")
    im = Image.open(path).convert("RGB")
    return im.size[0], im.size[1], im.tobytes()


def lum(px, o):
    return (0.2126 * px[o] + 0.7152 * px[o + 1] + 0.0722 * px[o + 2]) / 255.0


def box(w, h, px, thr, step):
    x0, x1, y0, y1, n = w, -1, h, -1, 0
    for y in range(0, h, step):
        base = y * w * 3
        for x in range(w):
            if lum(px, base + x * 3) > thr:
                n += 1
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y < y0:
                    y0 = y
                if y > y1:
                    y1 = y
    return x0, x1, y0, y1, n, len(range(0, h, step)) * w


def sweep(path, step, quiet=False):
    w, h, px = load(path)
    if not quiet:
        print(f"{path}  {w}x{h}  (every {step}th row sampled)")
    boxes = []
    for thr in THRESHOLDS:
        x0, x1, y0, y1, n, tested = box(w, h, px, thr, step)
        boxes.append((x0, x1))
        if quiet:
            continue
        if n == 0:
            print(f"  thr {thr:.3f}: NO pixel of {tested} sampled exceeds it. "
                  f"There is no box here, rather than an empty one.")
            continue
        print(f"  thr {thr:.3f}: x {x0}..{x1} ({x1-x0+1}/{w})  "
              f"y {y0}..{y1} ({y1-y0+1}/{h})  "
              f"{n}/{tested} px above ({100.0*n/tested:.1f}%)")
    if not quiet:
        nrow = len(range(0, h, step))
        cols = [0.0] * w
        for y in range(0, h, step):
            base = y * w * 3
            for x in range(w):
                cols[x] += lum(px, base + x * 3)
        bins = [f"{1000.0*sum(cols[b:b+32])/(len(cols[b:b+32])*nrow):.0f}"
                for b in range(0, w, 32)]
        print("  column mean luminance x1000, 32-column bins:")
        print("    " + " ".join(bins))
        wide = [b for b in boxes if b[1] - b[0] + 1 >= w * 0.98]
        tight = [b for b in boxes if 0 <= b[1] and b[1] - b[0] + 1 < w * 0.9]
        if wide and tight:
            print("  VERDICT: the box MOVES with the threshold -- from the full "
                  "width down to a sub-rectangle. This is a bright subject in a "
                  "darker rendered frame, NOT an inset. Any single-threshold "
                  "box taken from this image describes the subject's extent.")
        elif tight and not wide:
            print("  VERDICT: the box HOLDS below the full width at EVERY "
                  "threshold, including the lowest. Nothing is rendered outside "
                  "it, so this is real geometry.")
        elif wide and not tight:
            print("  VERDICT: full width at every threshold. No inset, and no "
                  "dark surround either.")
    return w, h, px, boxes


def compare(a_p, b_p, step):
    aw, ah, ap = load(a_p)
    bw, bh, bp = load(b_p)
    print(f"A {a_p}  {aw}x{ah}")
    print(f"B {b_p}  {bw}x{bh}")
    if (aw, ah) != (bw, bh):
        print("DIFFERENT SIZES -- no pixel comparison is possible, and that "
              "difference is itself the answer.")
        return 1
    n = aw * ah
    idx = range(0, n, step)
    same = swapped = 0
    dsum = 0
    signed = 0
    for i in idx:
        o = i * 3
        r, g, b = ap[o], ap[o + 1], ap[o + 2]
        R, G, B = bp[o], bp[o + 1], bp[o + 2]
        if (r, g, b) == (R, G, B):
            same += 1
        if (r, g, b) == (B, G, R):
            swapped += 1
        dsum += abs(r - R) + abs(g - G) + abs(b - B)
        signed += (r - R) + (g - G) + (b - B)
    t = len(idx)
    print(f"sampled {t}/{n} pixels (step {step})")
    print(f"  identical:               {same}/{t} ({100.0*same/t:.1f}%)")
    print(f"  identical under RB swap: {swapped}/{t} ({100.0*swapped/t:.1f}%)")
    print(f"  mean |channel diff|: {dsum/(t*3):.2f} of 255")
    print(f"  mean SIGNED diff A-B: {signed/(t*3):+.2f} of 255  "
          f"(a constant offset here means a brightness difference, not a "
          f"geometric one)")
    if same == t:
        print("VERDICT: byte-identical -- one picture, two artefacts.")
    elif swapped == t:
        print("VERDICT: identical apart from a red/blue swap.")
    else:
        print("VERDICT: the two differ. Compare their sweeps before calling it "
              "geometric: a uniform signed offset is brightness.")
    return 0


def selftest(path, step):
    """The tool must report an inset when there IS one and must not when there
    is not, so both classes are driven from the SAME real frame."""
    w, h, px = load(path)
    q = bytearray(px)
    x0, x1 = w // 3, 2 * w // 3
    for y in range(h):
        base = y * w * 3
        for x in range(w):
            if x < x0 or x > x1:
                o = base + x * 3
                q[o] = q[o + 1] = q[o + 2] = 0
    print(f"POSITIVE case: {path} with every column outside {x0}..{x1} zeroed")
    boxes = []
    for thr in THRESHOLDS:
        bx0, bx1, _, _, n, tested = box(w, h, bytes(q), thr, step)
        boxes.append((bx0, bx1))
        print(f"  thr {thr:.3f}: x {bx0}..{bx1}  {n}/{tested} above")
    held = all(b[0] >= x0 and b[1] <= x1 for b in boxes if b[1] >= 0)
    print(f"  a real inset must hold inside {x0}..{x1} at EVERY threshold "
          f"-> {'PASS' if held else 'FAIL'}")

    print(f"NEGATIVE case: {path} unmodified")
    nb = []
    for thr in THRESHOLDS:
        bx0, bx1, _, _, n, tested = box(w, h, px, thr, step)
        nb.append((bx0, bx1))
        print(f"  thr {thr:.3f}: x {bx0}..{bx1}  {n}/{tested} above")
    moved = len({b for b in nb}) > 1
    print(f"  a subject-in-a-dark-frame must MOVE with the threshold "
          f"-> {'PASS' if moved else 'FAIL'}")
    ok = held and moved
    print(f"selftest: {'PASS' if ok else 'FAIL'} "
          f"(the discriminator was run against BOTH classes, not reasoned about)")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("sweep", "compare"), nargs="?",
                    default="sweep")
    ap.add_argument("images", nargs="*")
    ap.add_argument("--step", type=int, default=4,
                    help="sample every Nth row (and Nth pixel in compare)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if not a.images:
        raise SystemExit("REFUSING: no image given. NOTHING was measured.")
    if a.selftest:
        return selftest(a.images[0], a.step)
    if a.mode == "compare":
        if len(a.images) != 2:
            raise SystemExit("REFUSING: compare needs exactly two images.")
        return compare(a.images[0], a.images[1], a.step)
    rc = 0
    for p in a.images:
        sweep(p, a.step)
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main())
