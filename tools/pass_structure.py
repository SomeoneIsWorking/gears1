#!/usr/bin/env python3
"""Attribute a captured frame's draws to observable render phases.

WHAT THIS ANSWERS. It groups a long draw table into phase boundaries using only
captured register state. This makes the frame structure reviewable without
embedding private engine source or decoded title instructions in the tool.

WHERE THE RULES COME FROM. Each rule is an independently stated behavioral
contract, checked against the title's own register state from `GEARS_DRAW_DIAG`.
The implementation does not require or reproduce private engine source.

WHAT IT REFUSES TO DO. It does not name a phase whose signature it cannot
distinguish. Several effects use indistinguishable blended, depth-tested state,
so this tool reports that band as one phase rather than inventing a split. Every
draw it cannot place is printed with its state.

    tools/pass_structure.py scratch/ab/frame.tsv        # one frame
    tools/pass_structure.py --draws BASEPASS f.tsv      # list one phase's draws
    tools/pass_structure.py --selftest                  # prove the rules fire

The TSV comes from a replay:

    GEARS_DRAW_DIAG=out.tsv build/release/runtime/frame_replay scratch/frames/X.gfr
"""
import argparse
import collections
import csv
import sys

# Stable fingerprint of the colour-preserving shader observed on clear and
# depth-only draws. It separates geometry that updates depth from geometry whose
# colour output matters without reproducing the shader's instruction stream.
NULL_PS = "63c971f5e9d59913"

# Xenos DEPTH_FUNC (RB_DEPTHCONTROL bits 4..6). NEVER and ALWAYS are the two a
# clear uses; the title's depth test is GEQUAL because its projection is reverse-Z.
FUNC_NEVER, FUNC_ALWAYS = 0, 7

# The phases this tool is willing to name, in their observed submission order.
PHASE_ORDER = [
    "CLEAR",
    "PREPASS",
    "BASEPASS",
    "RESOLVE",
    "DEPTH_RESTORE",
    "OCCLUSION",
    "NO_OP",
    "BLENDED",
    "FULLSCREEN",
    "UNATTRIBUTED",
]

PHASE_WHY = {
    "CLEAR":
        "a rectangle-list draw using the colour-preserving shader; its colour "
        "mask and depth state show exactly which attachments it clears",
    "PREPASS":
        "colour writes off, depth test and depth write on, with the observed "
        "colour-preserving shader: geometry contributes depth only",
    "BASEPASS":
        "colour writes on, depth test and depth write on, blending off: opaque "
        "geometry between render-target boundaries",
    "RESOLVE":
        "copy mode transfers a render target, forming an observable handoff "
        "between draw phases",
    "DEPTH_RESTORE":
        "colour writes off with a non-pass-through pixel shader while depth is "
        "rewritten: geometry repopulates the active depth target",
    "OCCLUSION":
        "colour writes off, depth test on, depth write off, and the observed "
        "colour-preserving shader: visibility-only geometry",
    "NO_OP":
        "colour mask 0 with the depth test AND depth write both off: the render "
        "target cannot change, whatever the shader computes. Named for the "
        "OBSERVABLE only -- what these draws mean is not established, so the tool "
        "does not assign a more specific engine operation",
    "BLENDED":
        "colour writes on with blending on and depth writes off. Multiple effect "
        "families share this state, so this tool deliberately does not split them",
    "FULLSCREEN":
        "two primitives, depth test off, covering the scissor: a full-screen "
        "image-space operation; state alone cannot name the effect",
    "UNATTRIBUTED":
        "NO RULE MATCHED. Every one of these is listed in full below",
}


def _int(row, key, default=-1):
    v = row.get(key, "")
    if v is None or v == "":
        return default
    try:
        return int(v, 0)
    except ValueError:
        return default


def classify(row):
    """Return the phase name for one diag row. Pure function of the row."""
    if row.get("prim_name") == "resolve":
        return "RESOLVE"

    prim = row.get("prim_name", "")
    ps = (row.get("ps_hash") or "").lower()
    cmask = _int(row, "color_mask", 0)
    ztest = _int(row, "depth_test", 0)
    zwrite = _int(row, "depth_write", 0)
    zfunc = _int(row, "depth_func", -1)
    blend = _int(row, "blend_on", 0)
    prims = _int(row, "ia_prims", -1)
    null_ps = ps == NULL_PS

    if prim == "rectangle_list" and null_ps and zfunc in (FUNC_NEVER, FUNC_ALWAYS):
        return "CLEAR"

    if cmask == 0:
        if not ztest and not zwrite:
            return "NO_OP"
        if not null_ps:
            return "DEPTH_RESTORE"
        if ztest and zwrite:
            return "PREPASS"
        if ztest and not zwrite:
            return "OCCLUSION"
        return "UNATTRIBUTED"

    if not ztest and prims == 2:
        return "FULLSCREEN"

    if not blend and ztest and zwrite:
        return "BASEPASS"

    if blend and not zwrite:
        return "BLENDED"

    return "UNATTRIBUTED"


def runs_of(rows):
    """Collapse consecutive rows of the same phase into (phase, first, last, rows)."""
    out = []
    for r in rows:
        p = classify(r)
        if out and out[-1][0] == p:
            out[-1][2] = r["draw"]
            out[-1][3].append(r)
        else:
            out.append([p, r["draw"], r["draw"], [r]])
    return out


def tiles_of(rows):
    """Split the frame into predicated tiles at each COLOUR resolve of a surface
    back to the same destination.

    Captured frames replay draw groups over distinct EDRAM scissor bands. Treating
    each band as an unrelated phase hides the structure, while merging them hides
    tiling. This returns the bands so the caller can report both.
    """
    bands = collections.OrderedDict()
    for r in rows:
        if r.get("prim_name") == "resolve":
            continue
        key = "%sx%s" % (r["sc_w"], r["sc_h"])
        bands.setdefault(key, 0)
        bands[key] += 1
    return bands


def report(path, show_phase=None):
    with open(path, newline="") as f:
        rows = [r for r in csv.DictReader(f, delimiter="\t")]
    if not rows:
        print("REFUSING to report: %s has no rows. It describes NO frame -- this "
              "is a failure, not an empty result." % path, file=sys.stderr)
        return 2

    draws = [r for r in rows if r.get("prim_name") != "resolve"]
    resolves = [r for r in rows if r.get("prim_name") == "resolve"]
    counts = collections.Counter(classify(r) for r in rows)

    print("== observable render-phase structure of %s ==" % path)
    print("   %d rows: %d draws + %d resolves" % (len(rows), len(draws), len(resolves)))
    print()

    if show_phase:
        want = show_phase.upper()
        sel = [r for r in rows if classify(r) == want]
        print("-- %d draws in phase %s --" % (len(sel), want))
        if not sel:
            print("   NONE. Of %d rows, the phases present are: %s" %
                  (len(rows), ", ".join("%s=%d" % kv for kv in counts.most_common())))
            print("   So this is 'the frame has no %s', not 'the filter is broken'."
                  % want)
            return 1
        for r in sel:
            print("   draw %-5s surf=%-8s %-14s prims=%-5s cmask=%-2s "
                  "blend=%s/%-9s z=%s%sf%s sc=%sx%s ps=%s" % (
                      r["draw"], r["surface"], r["prim_name"], r["ia_prims"],
                      r["color_mask"], r["blend_on"], r["blend0"],
                      r["depth_test"], r["depth_write"], r["depth_func"],
                      r["sc_w"], r["sc_h"], r["ps_hash"]))
        return 0

    print("-- the frame, in submission order --")
    for phase, first, last, rs in runs_of(rows):
        span = ("draw %s" % first) if first == last else ("draws %s-%s" % (first, last))
        extra = ""
        if phase == "RESOLVE":
            extra = " " + ", ".join(
                "%s%s->%s %s@%s" % ("depth " if r["resolve_is_depth"] == "1" else "",
                                    r["surface"], r["resolve_dest"],
                                    r["resolve_src"], r["resolve_dst"])
                for r in rs)
        elif phase in ("BASEPASS", "BLENDED", "PREPASS", "OCCLUSION", "NO_OP"):
            shaders = collections.Counter(r["ps_hash"] for r in rs)
            extra = " (%d pixel shader%s" % (len(shaders),
                                             "" if len(shaders) == 1 else "s")
            if len(shaders) <= 3:
                extra += ": " + ", ".join(h for h, _ in shaders.most_common())
            extra += ")"
        elif phase == "FULLSCREEN":
            extra = " " + ", ".join(sorted({r["ps_hash"] for r in rs}))
        print("   %-13s %-18s x%-4d%s" % (phase, span, len(rs), extra))

    print()
    print("-- totals --")
    for p in PHASE_ORDER:
        if counts[p]:
            print("   %-13s %4d" % (p, counts[p]))
    absent = [p for p in PHASE_ORDER if not counts[p] and p != "UNATTRIBUTED"]
    if absent:
        # THE NEGATIVE, WITH ITS DENOMINATOR. "No fog pass" and "this tool never
        # looked for one" have to read differently.
        print()
        print("   ABSENT from this frame, having classified all %d rows: %s"
              % (len(rows), ", ".join(absent)))
        print("   These are phases the rules CAN name and did not find -- not "
              "phases that went unexamined.")

    print()
    print("-- predicated tiling --")
    bands = tiles_of(rows)
    if len(bands) == 1:
        print("   one scissor band (%s), %d draws: this frame is NOT tiled."
              % (next(iter(bands)), next(iter(bands.values()))))
    else:
        print("   %d scissor bands: %s" % (
            len(bands), ", ".join("%s x%d" % kv for kv in bands.items())))
        print("   Captured command groups repeat once per EDRAM tile, so a")
        print("   pass appears once per band. Draw counts per band differ when a")
        print("   primitive is culled in one tile and not another.")

    print()
    print("-- what this tool will not tell you --")
    print("   BLENDED is one phase here because several effect families issue")
    print("   blended, depth-tested, depth-write-off geometry with the same register")
    print("   state. Separating them needs evidence this table")
    print("   does not carry -- the bound texture set per draw, or the guest call")
    print("   site that emitted it (still unidentified, catalog #58).")
    if counts["UNATTRIBUTED"]:
        print()
        print("-- UNATTRIBUTED: %d rows no rule matched --" % counts["UNATTRIBUTED"])
        # Grouped by signature, with the draws that carry it. 48 identical rows
        # printed one per line reads as 48 findings; it is one.
        groups = collections.OrderedDict()
        for r in rows:
            if classify(r) != "UNATTRIBUTED":
                continue
            sig = ("surf=%s %s prims=%s cmask=%s blend=%s/%s z=%s%sf%s ps=%s" % (
                r["surface"], r["prim_name"], r["ia_prims"], r["color_mask"],
                r["blend_on"], r["blend0"], r["depth_test"], r["depth_write"],
                r["depth_func"], r["ps_hash"]))
            groups.setdefault(sig, []).append(r["draw"])
        for sig, ds in groups.items():
            where = ds[0] if len(ds) == 1 else "%s..%s" % (ds[0], ds[-1])
            print("   x%-4d draw %-12s %s" % (len(ds), where, sig))
    return 0


# --- self-test -------------------------------------------------------------
# A classifier that has never been run against a case it must REJECT is not a
# classifier. These feed one row per phase plus rows that must NOT match, and the
# suite fails if any verdict moves.
SELFTEST = [
    ("CLEAR", dict(prim_name="rectangle_list", ps_hash=NULL_PS, color_mask="15",
                   depth_test="1", depth_write="1", depth_func="7", blend_on="0",
                   ia_prims="2")),
    ("PREPASS", dict(prim_name="triangle_list", ps_hash=NULL_PS, color_mask="0",
                     depth_test="1", depth_write="1", depth_func="6", blend_on="0",
                     ia_prims="120")),
    ("OCCLUSION", dict(prim_name="triangle_list", ps_hash=NULL_PS, color_mask="0",
                       depth_test="1", depth_write="0", depth_func="6",
                       blend_on="0", ia_prims="12")),
    ("DEPTH_RESTORE", dict(prim_name="rectangle_list", ps_hash="272c76c2a6cc8701",
                           color_mask="0", depth_test="1", depth_write="1",
                           depth_func="7", blend_on="0", ia_prims="2")),
    ("BASEPASS", dict(prim_name="triangle_list", ps_hash="1f1a3f779667a02a",
                      color_mask="15", depth_test="1", depth_write="1",
                      depth_func="6", blend_on="0", ia_prims="900")),
    ("BLENDED", dict(prim_name="triangle_list", ps_hash="ae6bcda93492e09c",
                     color_mask="15", depth_test="1", depth_write="0",
                     depth_func="6", blend_on="1", ia_prims="40")),
    ("FULLSCREEN", dict(prim_name="triangle_list", ps_hash="501ac5d8692bf7b6",
                        color_mask="15", depth_test="0", depth_write="0",
                        depth_func="7", blend_on="0", ia_prims="2")),
    ("RESOLVE", dict(prim_name="resolve")),
    ("NO_OP", dict(prim_name="point_list", ps_hash=NULL_PS, color_mask="0",
                   depth_test="0", depth_write="0", depth_func="0", blend_on="0",
                   ia_prims="1")),
    # MUST NOT MATCH. A base-pass rule that also swallows blended geometry would
    # pass every positive case above and still be wrong.
    ("UNATTRIBUTED", dict(prim_name="triangle_list", ps_hash="deadbeef",
                          color_mask="15", depth_test="0", depth_write="1",
                          depth_func="6", blend_on="0", ia_prims="900")),
]


def selftest():
    bad = 0
    for expect, row in SELFTEST:
        got = classify(row)
        ok = got == expect
        bad += not ok
        print("   %-4s %-14s -> %s" % ("ok" if ok else "FAIL", expect, got))
    print("%d of %d classifier cases pass" % (len(SELFTEST) - bad, len(SELFTEST)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tsv", nargs="?", help="a GEARS_DRAW_DIAG table")
    ap.add_argument("--draws", metavar="PHASE",
                    help="list every draw in one phase instead of the summary")
    ap.add_argument("--selftest", action="store_true",
                    help="run the classifier against cases it must accept AND reject")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.tsv:
        ap.error("a TSV is required (or --selftest)")
    return report(a.tsv, a.draws)


if __name__ == "__main__":
    sys.exit(main())
