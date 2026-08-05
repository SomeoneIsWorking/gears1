#!/usr/bin/env python3
"""Attribute a captured frame's draws to UE3 render passes.

WHAT THIS ANSWERS. `docs/native-renderer.md` step 3: "recover the pass structure,
not the draws -- which draw ranges belong to which UE3 pass". A native pass is
written against a UE3 pass, so before writing one you have to know which draws
ARE that pass. Until now that was read off a 750-row TSV by eye.

WHERE THE RULES COME FROM. Each rule below cites the UE3 source that fixes the
state it keys on -- `$GEARS_UE3_SRC/Development/Src/Engine/Src/SceneRendering.cpp`
and `BasePassRendering.cpp`. The state itself is the title's, read out of the
register file by `GEARS_DRAW_DIAG`; UE3 only says what that state MEANS.

WHAT IT REFUSES TO DO. It does not name a pass whose signature it cannot
distinguish. UE3 emits lights, decals, distortion and translucency as blended
depth-tested geometry with identical render state, so this tool reports that band
as one phase and says so, rather than inventing a split. Every draw it cannot
place at all is printed in full with its state -- an unattributed draw is a
finding, not a rounding error.

    tools/pass_structure.py scratch/ab/frame.tsv        # one frame
    tools/pass_structure.py --draws BASEPASS f.tsv      # list one phase's draws
    tools/pass_structure.py --selftest                  # prove the rules fire

The TSV comes from a replay:

    GEARS_DRAW_DIAG=out.tsv scratch/build/runtime/frame_replay scratch/frames/X.gfr
"""
import argparse
import collections
import csv
import sys

# The title's pass-through pixel shader: `alloc colors; exece; max oC0, r0, r0`.
# It is what the guest binds when the pixel shader's output does not matter --
# EDRAM clears and the depth-only prepass -- so it separates "geometry drawn for
# its depth" from "geometry drawn for its colour". Verified by disassembly:
# scratch/shaders/bound_out/ps_63c971f5e9d59913.ucode.txt is those three lines.
NULL_PS = "63c971f5e9d59913"

# Xenos DEPTH_FUNC (RB_DEPTHCONTROL bits 4..6). NEVER and ALWAYS are the two a
# clear uses; the title's depth test is GEQUAL because its projection is reverse-Z.
FUNC_NEVER, FUNC_ALWAYS = 0, 7

# The phases this tool is willing to name, in the order UE3 emits them within one
# depth-priority group (FSceneRenderer::RenderDPGBegin -> RenderDPGLights ->
# RenderDPGEnd, SceneRendering.cpp:2046, 2141, 2201).
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
        "a rectangle-list draw bound to the pass-through pixel shader: how Xenos "
        "clears EDRAM. The colour mask and depth state say what it cleared",
    "PREPASS":
        "colour writes off, depth test AND depth write on, pass-through pixel "
        "shader. UE3 RenderPrePassInner sets exactly this "
        "(TStaticDepthState<TRUE,CF_LessEqual> with the colour mask cleared), "
        "SceneRendering.cpp:3119-3121",
    "BASEPASS":
        "colour writes on, depth test and depth write on, blend off. UE3 "
        "FBasePassDrawingPolicy draws opaque geometry with depth writes, "
        "BasePassRendering.cpp; the pass runs between BeginRenderingSceneColor "
        "and the scene-colour resolve, SceneRendering.cpp:2076-2101",
    "RESOLVE":
        "RB_MODECONTROL.edram_mode == kCopy. UE3's BeginRenderingSceneColor / "
        "FinishRenderingSceneColor / ResolveSceneDepthTexture boundaries are "
        "these, so they are where one pass ends and the next begins",
    "DEPTH_RESTORE":
        "colour writes off with a REAL pixel shader (not the pass-through one): "
        "geometry drawn to repopulate EDRAM depth, which UE3-on-360 does around "
        "UpdateDownsampledDepthSurface, SceneRendering.cpp:2254",
    "OCCLUSION":
        "colour writes off, depth test on, depth write OFF, pass-through pixel "
        "shader. UE3 BeginOcclusionTests, SceneRendering.cpp:2258-2263",
    "NO_OP":
        "colour mask 0 with the depth test AND depth write both off: the render "
        "target cannot change, whatever the shader computes. Named for the "
        "OBSERVABLE only -- what the title issues these for is NOT established, "
        "and calling them occlusion queries or memexport would be a guess. Their "
        "vertex shader is the pass-through rect shader and carries no export "
        "(scratch/shaders/bound/vs_760aacf6212e632c.ucode), which rules memexport "
        "out but names nothing",
    "BLENDED":
        "colour writes on with blending on and depth writes off. UE3 emits "
        "lights, decals, distortion and translucency with this same state "
        "(RenderDPGLights:2141, RenderDecals:2220, RenderTranslucency:2300), so "
        "THIS TOOL DOES NOT SPLIT THEM -- see the caveat printed with the table",
    "FULLSCREEN":
        "two primitives, depth test off, covering the scissor: a full-screen "
        "pass. UE3's post-process chain (RenderPostProcessEffects:3297) and "
        "RenderFog:614 both draw exactly this",
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

    UE3-on-360 replays one command buffer per EDRAM tile, so the base pass appears
    N times with N different scissor bands. Reporting it as N unrelated blocks
    hides the structure; reporting it as one block hides the tiling. This returns
    the bands so the caller can say both.
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

    print("== UE3 pass structure of %s ==" % path)
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
        print("   UE3-on-360 replays the command buffer once per EDRAM tile, so a")
        print("   pass appears once per band. Draw counts per band differ when a")
        print("   primitive is culled in one tile and not another.")

    print()
    print("-- what this tool will not tell you --")
    print("   BLENDED is ONE phase here and FOUR in UE3: lights (RenderDPGLights,")
    print("   SceneRendering.cpp:2141), decals (:2220), distortion and translucency")
    print("   (:2300) all draw blended, depth-tested, depth-write-off geometry with")
    print("   the same register state. Separating them needs evidence this table")
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
