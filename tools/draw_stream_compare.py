#!/usr/bin/env python3
"""Compare WHAT THE GUEST ASKS THE GPU TO DO, our runtime against the oracle.

    tools/draw_stream_compare.py <ours.tsv> <theirs.tsv>

Produced by `GEARS_DRAW_STREAM=<path>` (our runtime) and
`GEARS_ORACLE_DRAW_STREAM=<path>` (the Xenia fork). One line per presented
frame: `<frame>\\t<draws>\\t<vs>:<ps>:<count>...`, where the hashes are FNV-1a 64
over the big-endian shader microcode on BOTH sides.

WHY THIS COMPARISON AND NOT A PIXEL DIFF. Catalog #84 establishes that no clock
anchor makes two runs reach the same state at the same frame while the guest's
threads are scheduled by the host -- four were tried and the guest's own work
count matches at 1 of 22,209 presents. So a frame-indexed pixel comparison
against the oracle is not available and will not become available without a
deterministic scheduler.

The draw stream needs none of that. It is the guest's OWN OUTPUT -- what our CPU
emulation computed and handed to the GPU -- so a difference between our
emulation and the console's shows up as work requested differently. And the
questions worth asking of it do not need the two sides to be at the same moment:

  * which shaders does one side EVER bind that the other never does? No
    alignment at all: a set difference over the whole run.
  * for a shader both sides bind, how do the per-frame draw counts compare?
    Compared as DISTRIBUTIONS, not frame by frame.

WHAT THIS CANNOT SEE, stated because a clean report here is not a clean bill of
health: identical draw calls carrying different CONSTANTS, different vertex data,
or different textures. It compares which shader ran and how often, nothing more.
A pass that is present on both sides and wrong on ours looks identical here.
"""
import sys
from collections import Counter, defaultdict
from pathlib import Path


def load(path):
    """frames: list of (frame_index, draw_count, Counter{(vs,ps): n})."""
    frames = []
    for line in path.read_text(errors="replace").splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        try:
            idx, draws = int(parts[0]), int(parts[1])
        except ValueError:
            continue
        counts = Counter()
        for field in parts[2:]:
            bits = field.split(":")
            if len(bits) != 3:
                continue
            counts[(bits[0], bits[1])] += int(bits[2])
        frames.append((idx, draws, counts))
    return frames


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    ours_path, theirs_path = Path(argv[1]), Path(argv[2])
    for p in (ours_path, theirs_path):
        if not p.is_file():
            print(f"REFUSING: {p} does not exist. Nothing was compared.")
            return 1

    ours, theirs = load(ours_path), load(theirs_path)
    if not ours or not theirs:
        # A side with no frames is a run that did not record, NOT a run that
        # requested no work. Refusing keeps those two apart.
        print(f"REFUSING: {ours_path.name} has {len(ours)} frames and "
              f"{theirs_path.name} has {len(theirs)}. A side with none did not "
              f"record; nothing was compared.")
        return 1

    print(f"ours   {len(ours):>6} frames, {sum(f[1] for f in ours):>9} draws")
    print(f"theirs {len(theirs):>6} frames, {sum(f[1] for f in theirs):>9} draws")

    ours_total, theirs_total = Counter(), Counter()
    ours_frames, theirs_frames = defaultdict(int), defaultdict(int)
    for _, _, c in ours:
        ours_total += c
        for k in c:
            ours_frames[k] += 1
    for _, _, c in theirs:
        theirs_total += c
        for k in c:
            theirs_frames[k] += 1

    # THE MOMENT-INSENSITIVE QUESTION, and the only one here that needs no
    # alignment: which shader pairs does one side ever bind and the other never?
    only_ours = set(ours_total) - set(theirs_total)
    only_theirs = set(theirs_total) - set(ours_total)
    both = set(ours_total) & set(theirs_total)

    print(f"\ndistinct (vs, ps) pairs: ours {len(ours_total)}, "
          f"theirs {len(theirs_total)}, shared {len(both)}")

    def show(title, keys, totals, frames, n=15):
        # ALWAYS prints, including the empty case: "(none)" and a section that
        # was never reached must not look the same.
        print(f"\n{title}: {len(keys)}")
        if not keys:
            print("  (none — every pair this side bound, the other bound too)")
            return
        for k in sorted(keys, key=lambda k: -totals[k])[:n]:
            print(f"  vs {k[0]}  ps {k[1]}  {totals[k]:>8} draws in "
                  f"{frames[k]:>5} frames")
        if len(keys) > n:
            print(f"  ... and {len(keys) - n} more")

    show("ONLY OURS — pairs the oracle NEVER binds", only_ours, ours_total,
         ours_frames)
    show("ONLY THEIRS — pairs we NEVER bind", only_theirs, theirs_total,
         theirs_frames)

    # For shared pairs, compare the per-frame rate rather than the total: the
    # two runs are different lengths and reach different points, so totals are
    # not comparable and a ratio of them would look like a finding.
    print("\nSHARED PAIRS, draws per frame in which the pair appears "
          "(ours vs theirs):")
    rows = []
    for k in both:
        a = ours_total[k] / max(ours_frames[k], 1)
        b = theirs_total[k] / max(theirs_frames[k], 1)
        rows.append((abs(a - b) / max(a, b, 1e-9), a, b, k))
    rows.sort(reverse=True)
    print(f"  {'vs':>16} {'ps':>16} {'ours/fr':>9} {'theirs/fr':>10}  ratio")
    for rel, a, b, k in rows[:15]:
        print(f"  {k[0]:>16} {k[1]:>16} {a:>9.2f} {b:>10.2f}  "
              f"{(a / b if b else float('inf')):.2f}x")
    close = sum(1 for rel, _, _, _ in rows if rel < 0.10)
    print(f"\n  {close} of {len(rows)} shared pairs agree within 10% on "
          f"draws-per-frame")

    print("\nBLIND SPOT: this compares WHICH shader ran and HOW OFTEN. It cannot "
          "see identical\ndraws carrying different constants, vertex data or "
          "textures — a pass present on both\nsides and wrong on ours looks "
          "identical here.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
