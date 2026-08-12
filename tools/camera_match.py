#!/usr/bin/env python3
"""Find which of our frames stood where the console stood.

THE ALIGNMENT PROBLEM, and the only key that has survived it. Both emulators
advance the guest by wall-clock delta, so the same frame INDEX is not the same
game time (catalog #98), the same DRAW ORDINAL is not the same draw (catalog
#91 measured our 19,776-vertex draw at ordinal 289 against the console's 294),
and a content predicate -- "N frames after the first with 400+ draws" -- lands
the two sides at different places in the level. Every one of those was tried
and every one produced a comparison of two different moments that then read as
a renderer difference.

The view-projection is different in kind. It is GUEST DATA: the title computes
it and writes it into the vertex constant file, both emulators carry it
unchanged, and it names the viewpoint directly. Two frames with the same
view-projection are the same moment no matter what index either side gave them.

This is what killed catalog #91's clip lead. Our per-shader post-clip totals
looked catastrophically wrong -- 235 primitives of 54,352 against the console's
21,296 -- until the frames were matched by camera instead of by content, at
which point our own numbers came out at 21,111 of 54,352, a 0.9% difference.

Usage:
  camera_match.py <our runtime log> <the oracle's vs_consts dump> [--vs HASH]

The negative matters as much as the match, so this always prints the full
distribution of distances: if our closest frame is far from theirs, the run
never reached that viewpoint and NOTHING in it should be compared.
"""
import argparse
import re
import sys

CONST = re.compile(r"c\[(\d+)\]=\(([^)]*)\)")
CAMERA_ROWS = (230, 231, 232, 233)


def read_theirs(path):
    """The oracle's dump: one constant per line, no header."""
    cam = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            for idx, floats in CONST.findall(line):
                i = int(idx)
                if i in CAMERA_ROWS:
                    v = tuple(float(p) for p in floats.split(","))
                    if len(v) == 4:
                        cam[i] = v
    return cam


def read_ours(path, vs_hash):
    """Every dump in our log that carries a full camera, in log order."""
    header = re.compile(
        r"draw (\d+) \(diag \d+\) vs 0x" + vs_hash + r" float constants \(")
    out, cur, draw, line_no = [], None, None, 0
    with open(path, "r", errors="replace") as f:
        for n, line in enumerate(f, 1):
            m = header.search(line)
            if m:
                cur, draw, line_no = {}, int(m.group(1)), n
            if cur is None or "c[" not in line:
                continue
            for idx, floats in CONST.findall(line):
                i = int(idx)
                if i in CAMERA_ROWS:
                    v = tuple(float(p) for p in floats.split(","))
                    if len(v) == 4:
                        cur[i] = v
            if all(i in cur for i in CAMERA_ROWS):
                out.append((line_no, draw, cur))
                cur = None
    return out


def distance(a, b):
    return max(max(abs(x - y) for x, y in zip(a[i], b[i])) for i in CAMERA_ROWS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("theirs")
    ap.add_argument("--vs", default="f3e9368c1bb68ecc",
                    help="vertex shader hash whose constant dumps to read")
    ap.add_argument("--near", type=float, default=10.0,
                    help="distance under which two frames count as the same "
                         "viewpoint; the residual still costs a little")
    ap.add_argument("--show", type=int, default=6)
    a = ap.parse_args()

    theirs = read_theirs(a.theirs)
    missing = [i for i in CAMERA_ROWS if i not in theirs]
    if missing:
        print(f"REFUSING: {a.theirs} has no camera -- constants {missing} are "
              f"absent. It is not a dump of a draw that uses one.",
              file=sys.stderr)
        return 2
    ours = read_ours(a.ours, a.vs)
    if not ours:
        print(f"REFUSING TO REPORT A MATCH: no dump of vertex shader {a.vs} in "
              f"{a.ours} carries a full camera. Either the run had no "
              f"GEARS_DRAW_VS_CONSTS_VS for it, or that shader was never bound.",
              file=sys.stderr)
        return 2

    scored = sorted(((distance(c, theirs), ln, dr) for ln, dr, c in ours))
    print(f"{len(scored)} dump(s) of {a.vs} in {a.ours} carry a camera.")
    print(f"distance to the console's camera: closest {scored[0][0]:.2f}, "
          f"median {scored[len(scored)//2][0]:.2f}, "
          f"furthest {scored[-1][0]:.2f}")
    for d, ln, dr in scored[:a.show]:
        print(f"  {d:9.2f}  {a.ours}:{ln}  draw {dr}")
    if scored[0][0] > a.near:
        print(f"\nNO FRAME OF OURS STOOD WHERE THE CONSOLE STOOD. The closest "
              f"is {scored[0][0]:.2f} away and the threshold is {a.near}. "
              f"Nothing in this run is comparable to that console frame -- not "
              f"post-clip counts, not resolve contents, not pass structure. "
              f"Capture more frames or a longer run.")
        return 1
    print(f"\nMatched at {scored[0][0]:.2f}, under the {a.near} threshold. Only "
          f"that frame is comparable, and the residual still shows up in any "
          f"viewpoint-sensitive quantity.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
