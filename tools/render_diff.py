#!/usr/bin/env python3
"""Find the FIRST draw at which two renders diverge.

    tools/render_diff.py <a.tsv> <b.tsv>        # first divergence, and context
    tools/render_diff.py --selftest             # prove it reports both answers

The inputs are written by `GEARS_DRAW_TRACE_ALL=<path.tsv>`: one row per issued
draw, carrying what the draw WAS (its diag index, surface and pixel shader) and
a hash plus per-channel statistics of a 32x18 thumbnail of the surface AFTER it.

WHY THIS EXISTS. "Which draw is the first to differ between these two runs" was
being answered by hand, one hypothesis per iteration, from single-pixel traces --
and twice the answer was attributed to the wrong draw, because a single pixel
cannot see a change somewhere else and a filtered trace skipped the draws in
between. Two runs under different knobs now answer it in one command.

WHAT A DIVERGENCE MEANS, AND WHAT IT DOES NOT. The first differing row is where
the two runs' surfaces stop matching. That is the draw to look at; it is not
proof that this draw is at fault, because a difference introduced earlier can be
invisible in a thumbnail until a later draw magnifies it. The report says how
many rows matched before it, which is the denominator that claim needs.
"""
import sys
from pathlib import Path


def read(path):
    p = Path(path)
    if not p.is_file():
        # A comparison against a file that does not exist compared NOTHING, and
        # must not be readable as "no differences".
        print(f"REFUSING: {p} does not exist. Nothing was compared.")
        raise SystemExit(2)
    rows = []
    with p.open() as f:
        header = f.readline().rstrip("\n").split("\t")
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) == len(header):
                rows.append(dict(zip(header, parts)))
    if not rows:
        print(f"REFUSING: {p} has a header but no rows. Nothing was compared.")
        raise SystemExit(2)
    return rows


def describe(r):
    return (f"draw {r['draw']:>4} (guest {r['diag']:>4})  surface {r['surface']:>6}"
            f"  ps {r['ps_hash'][:12]:>12}")


def stats(r):
    return (f"max {float(r['maxR']):8.4f} {float(r['maxG']):8.4f} {float(r['maxB']):8.4f}"
            f"   mean {float(r['meanR']):7.4f} {float(r['meanG']):7.4f} {float(r['meanB']):7.4f}")


def compare(a, b, name_a="A", name_b="B"):
    """Returns the index of the first differing row, or None."""
    n = min(len(a), len(b))
    for i in range(n):
        if a[i]["thumb_hash"] != b[i]["thumb_hash"]:
            return i
    if len(a) != len(b):
        return n
    return None


def main(argv):
    args = argv[1:]
    if args and args[0] == "--selftest":
        return selftest()
    if len(args) != 2:
        print(__doc__)
        return 2
    a, b = read(args[0]), read(args[1])
    print(f"== {args[0]}: {len(a)} rows   vs   {args[1]}: {len(b)} rows ==")

    # The draw streams themselves must line up, or "the same draw" means nothing.
    # This is checked rather than assumed: a knob that changes which draws are
    # ISSUED (the tiling collapse, a skip) shifts every row after it, and every
    # row would then differ for a reason that has nothing to do with rendering.
    misaligned = None
    for i in range(min(len(a), len(b))):
        if a[i]["diag"] != b[i]["diag"]:
            misaligned = i
            break
    if misaligned is not None:
        print(f"\n   THE DRAW STREAMS DIVERGE at row {misaligned}: "
              f"{args[0]} has guest draw {a[misaligned]['diag']}, "
              f"{args[1]} has {b[misaligned]['diag']}.")
        print("   The two runs are not issuing the same draws, so a per-row")
        print("   comparison after this point compares different draws. Any")
        print("   pixel difference below is NOT attributable to a draw.")

    i = compare(a, b)
    if i is None:
        print(f"\n   IDENTICAL: all {len(a)} rows match on the thumbnail hash.")
        print("   That is the whole frame, not a sample of it -- but a 32x18")
        print("   thumbnail cannot see a difference smaller than one of its")
        print("   texels, so this is 'no difference this can resolve'.")
        return 0
    if i >= len(a) or i >= len(b):
        print(f"\n   One run is longer: they match for all {min(len(a), len(b))}"
              f" shared rows and then one continues.")
        return 1

    print(f"\n   FIRST DIVERGENCE at row {i} -- {i} row(s) matched before it")
    print(f"   {describe(a[i])}")
    print(f"     {args[0]}: {stats(a[i])}")
    print(f"     {args[1]}: {stats(b[i])}")
    lo = max(0, i - 3)
    print(f"\n   context, the {i - lo} row(s) that still matched:")
    for k in range(lo, i):
        print(f"     {describe(a[k])}  {stats(a[k])}")
    print("\n   The first differing row is where the surfaces stop matching. It")
    print("   is the draw to look at, NOT proof that this draw is at fault: a")
    print("   difference introduced earlier can hide in a thumbnail until a")
    print("   later draw magnifies it.")
    return 1


def selftest():
    """Run against BOTH classes: files that differ, and files that do not."""
    import tempfile, os
    hdr = ("draw\tdiag\tsurface\tps_hash\tthumb_hash"
           "\tmaxR\tmaxG\tmaxB\tmeanR\tmeanG\tmeanB\n")

    def write(rows):
        fd, path = tempfile.mkstemp(suffix=".tsv")
        with os.fdopen(fd, "w") as f:
            f.write(hdr)
            for r in rows:
                f.write("\t".join(str(x) for x in r) + "\n")
        return path

    base = [[i, 600 + i, "0x2d0", "abc", f"{i:016x}", 1, 1, 1, 0.5, 0.5, 0.5]
            for i in range(10)]
    same = write(base)
    same2 = write(base)
    changed = [list(r) for r in base]
    changed[6][4] = "deadbeefdeadbeef"
    diff = write(changed)
    shifted = [list(r) for r in base]
    shifted[5][1] = 999
    shift = write(shifted)

    ok = True
    a, b = read(same), read(same2)
    if compare(a, b) is not None:
        print("FAIL: identical files reported a divergence"); ok = False
    a, b = read(same), read(diff)
    if compare(a, b) != 6:
        print(f"FAIL: divergence at row 6 reported as {compare(a, b)}"); ok = False
    # The misalignment check must fire on a shifted draw stream.
    a, b = read(same), read(shift)
    mis = next((i for i in range(len(a)) if a[i]["diag"] != b[i]["diag"]), None)
    if mis != 5:
        print(f"FAIL: draw-stream misalignment at row 5 reported as {mis}"); ok = False
    for p in (same, same2, diff, shift):
        os.unlink(p)
    # A missing file must REFUSE rather than report no differences.
    try:
        read("/nonexistent/render_diff_selftest.tsv")
        print("FAIL: a missing file did not refuse"); ok = False
    except SystemExit as e:
        if e.code != 2:
            print(f"FAIL: missing file exited {e.code}, expected 2"); ok = False
    print("selftest: PASS -- reports a difference, reports NO difference, "
          "catches a shifted draw stream, and refuses a missing file"
          if ok else "selftest: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
