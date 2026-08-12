#!/usr/bin/env python3
"""Join the console's vertex float constants for ONE NAMED DRAW against ours.

Catalog #91 reaches a point where both emulators issue the same draw with the
same registers and only one of them rasterises it, and an independent hand
evaluation of the microcode agrees with OUR result -- so the arithmetic is not
the difference and the INPUTS have to be compared directly. This is that
comparison for the constants, which are the only per-bind input: the same
shader is bound six times in the frame with a different bone palette each time,
so the two dumps must be joined on the DRAW ORDINAL, not on the shader hash.

Ours comes from GEARS_DRAW_VS_CONSTS_VS in the runtime's log, theirs from
GEARS_ORACLE_VS_CONSTS_ORDINAL in the oracle. Both print guest constant
indices.

THE NEGATIVE IS THE POINT. "no differences" is only meaningful with its
denominator and its blind spots, so this always prints how many constants each
side carried, how many were COMPARED, and which indices only one side has. A
run that parses nothing exits non-zero rather than reporting agreement.
"""
import argparse
import re
import struct
import sys

# c[12]=(0.1, -2, 3, 4)  with an optional [hex hex hex hex] tail (ours)
CONST = re.compile(
    r"c\[(\d+)\]=\(([^)]*)\)(?:\[([0-9a-fA-F ]+)\])?")


def parse(path, draw=None):
    """Return {index: (x, y, z, w)} and a line count, from either dump."""
    vals, lines_used = {}, 0
    with open(path, "r", errors="replace") as f:
        for line in f:
            if draw is not None and "c[" in line and "draw" in line:
                # Ours interleaves every draw's dump in one log; take only the
                # requested one. Without this the last draw silently wins.
                m = re.search(r"draw (\d+) ", line)
                if m and int(m.group(1)) != draw:
                    continue
            hits = CONST.findall(line)
            if not hits:
                continue
            lines_used += 1
            for idx, floats, hexes in hits:
                if hexes:
                    words = hexes.split()
                    v = tuple(struct.unpack(">f", bytes.fromhex(w))[0]
                              for w in words)
                else:
                    v = tuple(float(p) for p in floats.split(","))
                if len(v) == 4:
                    vals[int(idx)] = v
    return vals, lines_used


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("theirs")
    ap.add_argument("--draw", type=int, default=None,
                    help="our draw ordinal, when the log holds several")
    ap.add_argument("--tol", type=float, default=1e-4)
    a = ap.parse_args()

    ours, ol = parse(a.ours, a.draw)
    theirs, tl = parse(a.theirs)
    print(f"ours:   {len(ours)} constants from {ol} line(s) of {a.ours}"
          + (f" (draw {a.draw})" if a.draw is not None else ""))
    print(f"theirs: {len(theirs)} constants from {tl} line(s) of {a.theirs}")
    if not ours or not theirs:
        print("REFUSING TO REPORT AGREEMENT: one side parsed to NOTHING, which "
              "is a broken dump or a wrong path, not two matching inputs.",
              file=sys.stderr)
        return 2

    only_ours = sorted(set(ours) - set(theirs))
    only_theirs = sorted(set(theirs) - set(ours))
    shared = sorted(set(ours) & set(theirs))
    diffs = []
    for i in shared:
        d = max(abs(x - y) for x, y in zip(ours[i], theirs[i]))
        if d > a.tol:
            diffs.append((d, i))
    diffs.sort(reverse=True)

    print(f"compared {len(shared)} constant(s) present on BOTH sides; "
          f"{len(only_ours)} only ours, {len(only_theirs)} only theirs")
    if only_ours:
        print(f"  only ours:   {only_ours[:16]}"
              + (" ..." if len(only_ours) > 16 else ""))
    if only_theirs:
        print(f"  only theirs: {only_theirs[:16]}"
              + (" ..." if len(only_theirs) > 16 else ""))
    print(f"{len(diffs)} of {len(shared)} differ by more than {a.tol}")
    for d, i in diffs[:40]:
        print(f"  c[{i}] max|delta|={d:g}")
        print(f"    ours   = {ours[i]}")
        print(f"    theirs = {theirs[i]}")
    if len(diffs) > 40:
        print(f"  ... and {len(diffs) - 40} more, NOT shown")
    if not diffs:
        print("Every SHARED constant agrees. This says nothing about the "
              f"{len(only_ours) + len(only_theirs)} constant(s) only one side "
              "carried, nor about any input that is not a float constant.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
