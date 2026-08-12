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


def blocks(path, draw=None):
    """Every dump of `draw` in the log, in order, as [{index: (x,y,z,w)}].

    ONE PER FRAME, not one per file. Our runtime dumps the same draw ordinal on
    every frame it renders, so a log from a run of any length holds several,
    with DIFFERENT constants -- a 20-frame run of the clip watch held six dumps
    of draw 294 whose local transform and even the bone-stride constant c[4]
    disagree (3 in one, 4 in the rest), because they are different moments of
    the game. Merging them into one dict silently keeps the LAST, which is a
    frame nobody chose. Splitting on the dump's own header keeps them apart.
    """
    out, cur = [], None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = re.search(r"draw (\d+) ", line)
            if draw is not None and m and int(m.group(1)) != draw:
                continue
            hits = CONST.findall(line)
            if not hits:
                continue
            # A header (not a "continued from" line) starts a new dump.
            if "continued from" not in line or cur is None:
                if cur:
                    out.append(cur)
                cur = {}
            for idx, floats, hexes in hits:
                if hexes:
                    v = tuple(struct.unpack(">f", bytes.fromhex(w))[0]
                              for w in hexes.split())
                else:
                    v = tuple(float(p) for p in floats.split(","))
                if len(v) == 4:
                    cur[int(idx)] = v
    if cur:
        out.append(cur)
    return out


def parse(path, draw=None, occurrence=None):
    """One dump, named explicitly when the log holds more than one."""
    bs = blocks(path, draw)
    if not bs:
        return {}, 0
    if len(bs) > 1 and occurrence is None:
        raise SystemExit(
            f"REFUSING TO GUESS: {path} holds {len(bs)} separate dumps of this "
            f"draw -- one per frame, with different constants. Name one with "
            f"--ours-occurrence/--theirs-occurrence (1-based). Picking one "
            f"silently is how a comparison ends up joining two different "
            f"moments of the game and calling the result a renderer difference.")
    return bs[(occurrence or 1) - 1], len(bs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("theirs")
    ap.add_argument("--draw", type=int, default=None,
                    help="our draw ordinal, when the log holds several")
    ap.add_argument("--ours-occurrence", type=int, default=None,
                    help="which frame's dump of that draw, 1-based")
    ap.add_argument("--theirs-occurrence", type=int, default=None)
    ap.add_argument("--theirs-draw", type=int, default=None,
                    help="draw filter for the second file; the oracle's dump "
                         "holds one draw and needs none, but a self-test that "
                         "feeds our own log to both sides does")
    ap.add_argument("--tol", type=float, default=1e-4)
    a = ap.parse_args()

    ours, ol = parse(a.ours, a.draw, a.ours_occurrence)
    theirs, tl = parse(a.theirs, a.theirs_draw, a.theirs_occurrence)
    print(f"ours:   {len(ours)} constants, dump "
          f"{a.ours_occurrence or 1} of {ol} in {a.ours}"
          + (f" (draw {a.draw})" if a.draw is not None else ""))
    print(f"theirs: {len(theirs)} constants, dump "
          f"{a.theirs_occurrence or 1} of {tl} in {a.theirs}")
    print("THESE ARE ONLY COMPARABLE IF THEY ARE THE SAME GAME MOMENT. Both "
          "emulators advance the guest by wall-clock delta, so equal frame "
          "indices are not equal game time (catalog #98); the two sides must "
          "have been selected by the SAME content predicate.")
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
