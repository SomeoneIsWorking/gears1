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

    A DRAW THAT RUNS NO FRAGMENT STAGE IS KEYED ps=0, because that is what the
    console's record holds: Xenia binds no pixel shader for a depth-only draw
    and hashes a null shader to zero, while our table keeps the hash the guest
    bound. Without this the two sides look wildly different where they agree
    exactly -- measured, one vertex shader read as "ours 73, theirs 2" when the
    truth was 71 depth-only draws against 69 and 2 shaded against 2. That was
    this tool's first real finding and it was a false one.
    """
    lines = Path(path).read_text(errors="replace").splitlines()
    if not lines:
        return None, "the table is empty"
    cols = {n: i for i, n in enumerate(lines[0].split("\t"))}
    for need in ("vs_hash", "ps_hash", "frag_stage"):
        if need not in cols:
            return None, f"the table has no {need} column (it predates it)"
    # THE WIDE KEY when the table carries the raw registers, the narrow one when
    # it does not -- an older capture is still comparable, at less resolution,
    # and the caller is told which it got rather than left to assume.
    wide = all(c in cols for c in
               ("depth_control", "stencil_ref_mask_raw", "blend0"))
    counts = Counter()
    for line in lines[1:]:
        f = line.split("\t")
        try:
            vs, ps = f[cols["vs_hash"]].strip(), f[cols["ps_hash"]].strip()
        except IndexError:
            continue
        if not vs:
            continue                      # a resolve: no shader
        try:
            shaded = f[cols["frag_stage"]].strip() == "1"
        except (IndexError, KeyError):
            shaded = True                 # a table without the column: unchanged
        key = [vs.lower().lstrip("0") or "0",
               (ps.lower().lstrip("0") or "0") if shaded else "0"]
        if wide:
            for c in ("depth_control", "stencil_ref_mask_raw", "blend0"):
                try:
                    key.append(f[cols[c]].strip().lower().removeprefix("0x")
                               .lstrip("0") or "0")
                except IndexError:
                    key.append("0")
        counts[tuple(key)] += 1
    return counts, ('wide' if wide else 'narrow')


def read_theirs(path, frame=None):
    """Per-(vs,ps) draw counts from the oracle's per-frame draw stream.

    One line per frame, as the fork writes it:

        <frame>\t<draws recorded>\t<vs>:<ps>:<n>\t<vs>:<ps>:<n>...

    The second field is the frame's own draw TOTAL, and it is checked against
    the sum of the per-pair counts rather than ignored -- a reader that quietly
    drops cells it cannot parse would otherwise report a smaller frame as a
    difference in the renderer. This reader assumed a different separator at
    first and matched nothing; it refused rather than reporting an empty
    console, which is why that cost minutes instead of a wrong finding.

    Without --frame the BUSIEST frame is used, which is the gameplay frame the
    capture is about, and the choice is returned so the caller can print it.
    """
    best, best_frame, best_total, seen = None, None, 0, 0
    for line in Path(path).read_text(errors="replace").splitlines():
        f = line.split("\t")
        if len(f) < 3:
            continue
        seen += 1
        try:
            total = int(f[1])
        except ValueError:
            continue
        counts = Counter()
        for cell in f[2:]:
            parts = cell.split(":")
            # Six fields is the wide record (shaders plus RB_DEPTHCONTROL,
            # RB_STENCILREFMASK and RB_BLENDCONTROL0); three is the record
            # before the state was added. Anything else is not parsed, and the
            # total check below is what makes that visible.
            if len(parts) not in (3, 6):
                continue
            try:
                n = int(parts[-1])
            except ValueError:
                continue
            counts[tuple(p.lower().lstrip("0") or "0"
                         for p in parts[:-1])] += n
        if best is None or sum(counts.values()) > sum(best.values()):
            best, best_frame, best_total = counts, f[0], total
        if frame is not None and f[0] == str(frame):
            return counts, f[0], seen, total
    if frame is not None:
        return None, None, seen, 0
    return best, best_frame, seen, best_total


def key_str(k):
    s = f"vs {k[0]} ps {k[1]}"
    if len(k) == 5:
        s += f" depthctl {k[2]} stencil {k[3]} blend {k[4]}"
    return s


def compare(ours, theirs, out=print):
    shared = set(ours) & set(theirs)
    only_o = set(ours) - set(theirs)
    only_t = set(theirs) - set(ours)
    differing = sorted((k for k in shared if ours[k] != theirs[k]),
                       key=lambda k: -abs(ours[k] - theirs[k]))
    out(f"shader pairs: {len(shared)} on both sides,"
        f" {len(only_o)} only ours, {len(only_t)} only theirs")
    out(f"draws: {sum(ours.values())} ours, {sum(theirs.values())} theirs")
    # THE TILING COLLAPSE IS NOT A MISSING DRAW, and it is most of this table.
    # The console replays the command buffer once per predicated EDRAM tile --
    # this title's full-screen surface is two bands, 512 rows and 208 -- while
    # this renderer draws each thing once (GEARS_DRAW_TILED=1 restores the
    # faithful path). So `theirs == 2 * ours` is the DESIGN, and printing it
    # beside a genuine difference buries the second in the first: measured, 9 of
    # the 13 differing pairs in the first paired run were exactly 2x.
    tiled = [k for k in differing if theirs[k] == 2 * ours[k]]
    real = [k for k in differing if theirs[k] != 2 * ours[k]]
    if not differing:
        out(f"  none of the {len(shared)} shared pairs differs in count")
    if tiled:
        out(f"  {len(tiled)} pair(s) differ by EXACTLY 2x (theirs = 2 x ours),"
            f" which is the predicated tiling this renderer collapses, not a"
            f" missing draw -- listed last")
    for k in real:
        ratio = (f"{theirs[k] / ours[k]:.2f}x" if ours[k] else "ours has none")
        out(f"  {key_str(k)}: ours {ours[k]}, theirs {theirs[k]}"
            f"  ({ours[k] - theirs[k]:+d}, {ratio})")
    for k in tiled:
        out(f"  [2x, the tiling] {key_str(k)}:"
            f" ours {ours[k]}, theirs {theirs[k]}")
    # THE QUESTION THE STATE FIELDS EXIST FOR, asked directly instead of left to
    # be reconstructed from the lists above: for a shader BOTH sides run, do they
    # run it with the same set of depth/stencil/blend states? A renderer that
    # binds the wrong stencil ref or the wrong blend shows up here and nowhere
    # else -- in the raw key it looks like one pair only-ours and one
    # only-theirs, which is the same shape as a shader one side never ran.
    if any(len(k) == 5 for k in list(ours) + list(theirs)):
        o_by, t_by = {}, {}
        for k in ours:
            o_by.setdefault(k[:2], set()).add(k[2:])
        for k in theirs:
            t_by.setdefault(k[:2], set()).add(k[2:])
        both = set(o_by) & set(t_by)
        differing_state = sorted(s for s in both if o_by[s] != t_by[s])
        out(f"shaders BOTH sides run: {len(both)};"
            f" {len(both) - len(differing_state)} with exactly the same set of"
            f" draw states, {len(differing_state)} with a different set")
        for sh in differing_state:
            out(f"  vs {sh[0]} ps {sh[1]} runs with different STATE:")
            out(f"    ours  : {sorted(o_by[sh])}")
            out(f"    theirs: {sorted(t_by[sh])}")

    for label, s, src in (("only ours", only_o, ours), ("only theirs", only_t, theirs)):
        for k in sorted(s, key=lambda q: -src[q])[:12]:
            out(f"  {label}: {key_str(k)} x{src[k]}")
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
    lines = []
    compare(Counter({("aa", "bb"): 10, ("cc", "dd"): 3}),
            Counter({("aa", "bb"): 20, ("cc", "dd"): 4}), out=lines.append)
    tiled_ok = (any("1 pair(s) differ by EXACTLY 2x" in l for l in lines)
                and any("[2x, the tiling] vs aa" in l for l in lines)
                and any("ps dd: ours 3, theirs 4" in l for l in lines))
    print(f"selftest: an exactly-2x pair is separated as the tiling collapse"
          f" while a 4-vs-3 pair beside it is still reported as a difference:"
          f" {tiled_ok} (expected True)")
    # THE WIDE KEY, which is the whole point of the state fields: two draws of
    # the SAME shader pair with different depth/stencil state are different
    # draws, and if the key collapsed them a state difference would vanish into
    # an equal count. Here each side runs the same shaders the same number of
    # times and only the depth control differs -- which must read as one pair
    # only ours and one only theirs, not as agreement.
    lines = []
    d, o, t = compare(Counter({("aa", "bb", "1a", "0", "0"): 4}),
                      Counter({("aa", "bb", "2b", "0", "0"): 4}),
                      out=lines.append)
    state_ok = (not d and o == {("aa", "bb", "1a", "0", "0")}
                and t == {("aa", "bb", "2b", "0", "0")}
                and any("depthctl 1a" in l for l in lines)
                and any("depthctl 2b" in l for l in lines))
    state_ok = state_ok and any("runs with different STATE" in l for l in lines)
    print(f"selftest: same shaders and the same count with DIFFERENT depth"
          f" control read as one pair each side AND are named as one shader"
          f" running with different state: {state_ok} (expected True)")
    # ...AND THROUGH THE REAL READERS, on real file formats. The check above
    # exercises the comparison with keys handed to it; these two readers are
    # where a wide record silently becomes a narrow one (a missing column, a
    # cell with the wrong number of fields), and that failure looks like
    # agreement rather than an error.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / "ours.tsv").write_text(
            "draw\tvs_hash\tps_hash\tfrag_stage\tdepth_control"
            "\tstencil_ref_mask_raw\tblend0\n"
            # a shaded draw, a depth-only draw (ps must key as 0), and a draw
            # differing from the first ONLY in its stencil ref/mask
            "1\taa\tbb\t1\t0x1a\t0x2\t0x10001\n"
            "2\taa\tbb\t0\t0x1a\t0x2\t0x10001\n"
            "3\taa\tbb\t1\t0x1a\t0x9\t0x10001\n")
        (d / "theirs.tsv").write_text(
            "7\t3\t"
            "00000000000000aa:00000000000000bb:0000001a:00000002:00010001:1\t"
            "00000000000000aa:0000000000000000:0000001a:00000002:00010001:1\t"
            "00000000000000aa:00000000000000bb:0000001a:00000009:00010001:1\n")
        buf = []
        import io, contextlib
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = main(["pass_draws", "--ours", str(d / "ours.tsv"),
                       "--theirs", str(d / "theirs.tsv"), "--frame", "7"])
        text = out.getvalue()
        e2e_ok = (rc == 0 and "[wide key" in text
                  and "3 on both sides, 0 only ours, 0 only theirs" in text
                  and "none of the 3 shared pairs differs" in text)
        print(f"selftest: a wide table and a wide stream round-trip through the"
              f" REAL readers -- 3 pairs, none differing, and the depth-only"
              f" draw keyed ps 0 on both sides: {e2e_ok} (expected True)")
        # ...and the negative: drop the state columns from ours and the two
        # records must REFUSE rather than compare a wide key against a narrow one.
        (d / "narrow.tsv").write_text(
            "draw\tvs_hash\tps_hash\tfrag_stage\n1\taa\tbb\t1\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc2 = main(["pass_draws", "--ours", str(d / "narrow.tsv"),
                        "--theirs", str(d / "theirs.tsv"), "--frame", "7"])
        refuse_ok = rc2 == 1 and "REFUSING" in out.getvalue()
        print(f"selftest: a narrow table against a wide stream is REFUSED, not"
              f" compared to nothing: {refuse_ok} (expected True)")
    ok = same_ok and diff_ok and tiled_ok and state_ok and e2e_ok and refuse_ok
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
    ours, width = read_ours(args.ours)
    if ours is None:
        print(f"REFUSING: {args.ours}: {width}. Nothing was compared.")
        return 1
    theirs, frame, frames_seen, their_total = read_theirs(
        args.theirs, args.frame)
    if not theirs:
        print(f"REFUSING: {args.theirs} has no usable frame"
              + (f" numbered {args.frame}" if args.frame else "")
              + f" ({frames_seen} frame line(s) read). Nothing was compared.")
        return 1
    if not ours:
        print(f"REFUSING: {args.ours} holds no draws with a shader."
              f" Nothing was compared.")
        return 1
    # THE TWO RECORDS MUST BE THE SAME SHAPE. A wide table against a narrow
    # stream would share no key at all and report every pass as one-sided --
    # a total mismatch that reads exactly like a real divergence.
    their_width = "wide" if any(len(k) == 5 for k in theirs) else "narrow"
    if their_width != width:
        print(f"REFUSING: our table is the {width} key (shaders"
              + (" plus depth/stencil/blend state)" if width == "wide" else " only)")
              + f" and the console's stream is the {their_width} one. They share"
              f" no key, so every pass would read as one-sided. Re-capture both"
              f" sides with the current build. Nothing was compared.")
        return 1
    print(f"ours: {args.ours}  [{width} key: shaders"
          + (" + depth control, stencil ref/mask, blend]" if width == "wide"
             else " only]"))
    print(f"theirs: {args.theirs}, frame {frame} of {frames_seen} recorded"
          + ("" if args.frame else " (the busiest, which is the gameplay frame)"))
    # The stream's own total against what was parsed out of it: a cell this
    # reader could not read must not become a difference in the renderer.
    parsed = sum(theirs.values())
    if parsed != their_total:
        print(f"WARNING: that frame records {their_total} draws but only"
              f" {parsed} were parsed from its per-shader cells --"
              f" {their_total - parsed} unaccounted for, so a count difference"
              f" below may be this reader's and not the renderer's")
    compare(ours, theirs)
    print("\nNOTE: this counts DRAWS PER SHADER PAIR over a whole frame. It says"
          " whether the same geometry was submitted, NOT whether it landed in"
          " the same place -- a draw with the wrong transform counts the same.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
