#!/usr/bin/env python3
"""Does the console issue the same DRAWS as we do, per shader pair?

The layer comparison pairs the two emulators at every RESOLVE and says which
passes differ. When a pass's inputs all match and its output still does not --
which is where catalog #91 stands, with the scene depth, the HDR resolves and
every shadow-atlas tile agreeing while the shadow masks do not -- the next
question is not what the pass READ, it is what it DREW.

Both sides record that, with the same FNV-1a hash of the guest microcode:

  ours    GEARS_DRAW_DIAG's per-draw table, one row per draw (vs_hash, ps_hash)
  theirs  GEARS_ORACLE_DRAW_STREAM, per frame, counts per (vs, ps) pair

So a pass whose draws the port drops, duplicates, or issues against a different
shader shows up as a count that differs, keyed on something neither emulator
chose -- the guest's own microcode.

WHAT A NEGATIVE PRINTS, because that is the answer this will usually give: the
number of shader pairs compared, how many draws each side issued in total, and
every pair whose counts differ WITH both counts -- and when none differ, it says
so with the denominator ("57 pairs, 3,412 draws, none differing"), never a bare
"no differences". A pair present on one side only is reported as such rather
than as a count of zero, because "the console never ran this shader" and "we
never ran it" are different findings and both are interesting.

REFUSES rather than reporting nothing when either record is missing or carries
no frame the other has: a comparison of an empty table against a full one is not
a comparison.

    tools/pass_draws.py --ours <capture>/ours/draws.tsv \
                        --theirs <capture>/theirs_draws.tsv
    tools/pass_draws.py --selftest
"""
import argparse
import sys
from collections import Counter
from pathlib import Path


def read_ours(path):
    """Per-(vs,ps) draw counts from the runtime's diag table.

    Resolves are rows in that table too and are NOT draws; they carry no shader
    and are skipped by that fact rather than by their name.
    """
    lines = Path(path).read_text(errors="replace").splitlines()
    if not lines:
        return None, "the table is empty"
    cols = {n: i for i, n in enumerate(lines[0].split("\t"))}
    for need in ("vs_hash", "ps_hash"):
        if need not in cols:
            return None, f"the table has no {need} column (it predates it)"
    counts = Counter()
    for line in lines[1:]:
        f = line.split("\t")
        try:
            vs, ps = f[cols["vs_hash"]].strip(), f[cols["ps_hash"]].strip()
        except IndexError:
            continue
        if not vs:
            continue                      # a resolve: no shader
        counts[(vs.lower(), ps.lower() or "0")] += 1
    return counts, None


def read_theirs(path, frame=None):
    """Per-(vs,ps) draw counts from the oracle's per-frame draw stream.

    One line per frame: `<frame>\\t<vs>:<ps>=<n>\\t...`. Without --frame the
    BUSIEST frame is used, which is the gameplay frame the capture is about, and
    the choice is returned so the caller can print it.
    """
    best, best_frame, seen = None, None, 0
    for line in Path(path).read_text(errors="replace").splitlines():
        f = line.split("\t")
        if len(f) < 2:
            continue
        seen += 1
        counts = Counter()
        for cell in f[1:]:
            if "=" not in cell or ":" not in cell:
                continue
            pair, n = cell.rsplit("=", 1)
            vs, ps = pair.split(":", 1)
            try:
                counts[(vs.lower(), ps.lower())] += int(n)
            except ValueError:
                continue
        if best is None or sum(counts.values()) > sum(best.values()):
            best, best_frame = counts, f[0]
        if frame is not None and f[0] == str(frame):
            return counts, f[0], seen
    if frame is not None:
        return None, None, seen
    return best, best_frame, seen


def compare(ours, theirs, out=print):
    shared = set(ours) & set(theirs)
    only_o = set(ours) - set(theirs)
    only_t = set(theirs) - set(ours)
    differing = sorted((k for k in shared if ours[k] != theirs[k]),
                       key=lambda k: -abs(ours[k] - theirs[k]))
    out(f"shader pairs: {len(shared)} on both sides,"
        f" {len(only_o)} only ours, {len(only_t)} only theirs")
    out(f"draws: {sum(ours.values())} ours, {sum(theirs.values())} theirs")
    if not differing:
        out(f"  none of the {len(shared)} shared pairs differs in count")
    for k in differing:
        out(f"  vs {k[0]} ps {k[1]}: ours {ours[k]}, theirs {theirs[k]}"
            f"  ({ours[k] - theirs[k]:+d})")
    for label, s, src in (("only ours", only_o, ours), ("only theirs", only_t, theirs)):
        for k in sorted(s, key=lambda q: -src[q])[:12]:
            out(f"  {label}: vs {k[0]} ps {k[1]} x{src[k]}")
    return differing, only_o, only_t


def selftest():
    """Both classes, because a comparison that only ever says "same" is not one.

    An identical pair must report no differences AND its denominators; a pair
    differing by one draw must name it. The one-sided cases are checked too:
    they were the finding this tool exists to be able to make.
    """
    ok = True
    a = Counter({("aa", "bb"): 4, ("cc", "dd"): 1})
    lines = []
    d, o, t = compare(a, Counter(a), out=lines.append)
    same_ok = not d and not o and not t and any("none of the 2" in l for l in lines)
    print(f"selftest: identical records report no difference, with the"
          f" denominator: {same_ok} (expected True)")
    lines = []
    b = Counter({("aa", "bb"): 3, ("ee", "ff"): 2})
    d, o, t = compare(a, b, out=lines.append)
    diff_ok = (d == [("aa", "bb")] and o == {("cc", "dd")} and t == {("ee", "ff")}
               and any("ours 4, theirs 3" in l for l in lines)
               and any("only ours" in l for l in lines)
               and any("only theirs" in l for l in lines))
    print(f"selftest: a one-draw difference and both one-sided pairs are all"
          f" named: {diff_ok} (expected True)")
    ok = same_ok and diff_ok
    print("SELFTEST PASS" if ok else "SELFTEST FAIL: do not trust this tool")
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours")
    ap.add_argument("--theirs")
    ap.add_argument("--frame", help="the console frame to read (default: busiest)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv[1:])
    if args.selftest:
        return selftest()
    if not args.ours or not args.theirs:
        print("REFUSING: both --ours and --theirs are needed. Nothing was compared.")
        return 2
    for p in (args.ours, args.theirs):
        if not Path(p).is_file():
            print(f"REFUSING: {p} does not exist, so this run read NOTHING."
                  f" Nothing was compared.")
            return 1
    ours, why = read_ours(args.ours)
    if ours is None:
        print(f"REFUSING: {args.ours}: {why}. Nothing was compared.")
        return 1
    theirs, frame, frames_seen = read_theirs(args.theirs, args.frame)
    if not theirs:
        print(f"REFUSING: {args.theirs} has no usable frame"
              + (f" numbered {args.frame}" if args.frame else "")
              + f" ({frames_seen} frame line(s) read). Nothing was compared.")
        return 1
    if not ours:
        print(f"REFUSING: {args.ours} holds no draws with a shader."
              f" Nothing was compared.")
        return 1
    print(f"ours: {args.ours}")
    print(f"theirs: {args.theirs}, frame {frame} of {frames_seen} recorded"
          + ("" if args.frame else " (the busiest, which is the gameplay frame)"))
    compare(ours, theirs)
    print("\nNOTE: this counts DRAWS PER SHADER PAIR over a whole frame. It says"
          " whether the same geometry was submitted, NOT whether it landed in"
          " the same place -- a draw with the wrong transform counts the same.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
