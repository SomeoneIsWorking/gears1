#!/usr/bin/env python3
"""Where on screen did a SKINNED character's skeleton land?

`tools/clip_check.py` answers "is this draw genuinely outside the frustum" for
RIGID geometry and refuses skinned draws, because a skinned vertex shader keeps
a bone palette where that tool expects a world matrix. That refusal left catalog
#77's question unanswerable: when a character draw reports
`killed_by_clip_or_cull`, is the player off camera, or are we losing them?

This answers it only for registered constant layouts and refuses every other
shader. The supported layout was established from captured constant behavior:
three rows per bone begin at constant 8, the world transform uses constants
0..3, and the view-projection uses constants 233..236. Calibration against a
draw known to rasterize is mandatory because a wrong layout still produces
plausible coordinates.

WHAT THIS MEASURES, EXACTLY. It transforms each BONE's origin -- the point the
bone matrix maps (0,0,0) to -- through world and view-projection, and reports
where those joints land. It does NOT transform the mesh's vertices: a skinned
vertex is a weighted blend of several bones applied to a vertex OFFSET from
those joints, so the mesh extends around the skeleton by roughly a limb's
thickness. That makes this decisive for the gross case ("every joint is behind
the camera", "every joint is four screens to the left") and NOT decisive for a
character straddling the frustum edge. The output says which case it is rather
than leaving the reader to assume.

CALIBRATION IS MANDATORY, as in clip_check.py. Give it a draw the renderer says
it SHADED and the arithmetic must place that skeleton on screen; a run without
one prints UNCALIBRATED and computes nothing, because a swizzle read wrong here
produces confident coordinates, not an error.

    GEARS_DRAW_VS_CONSTS=460 frame_replay scratch/frames/bright.gfr > a.log
    GEARS_DRAW_VS_CONSTS=319 frame_replay scratch/frames/character_auto.gfr > b.log
    tools/skeleton_where.py a.log b.log --calibrate a.log
    tools/skeleton_where.py --selftest
"""
import argparse
import re
import sys
from pathlib import Path

# The one captured shader profile whose constant layout is registered.
KNOWN = {
    0x15CBC482459FE5B7: dict(
        world=(0, 1, 2, 3), viewproj=(233, 234, 235, 236), bones_at=8),
}

HDR_RE = re.compile(r"draw (\d+) \(diag (\d+)\) vs 0x([0-9a-f]+) float constants")
VEC_RE = re.compile(r"c\[(\d+)\]=\(([^)]*)\)")


def parse(text):
    """-> {diag: {'vs': hash, 'c': {i: (x,y,z,w)}}}"""
    out = {}
    cur = None
    for line in text.splitlines():
        m = HDR_RE.search(line)
        if m:
            cur = int(m.group(2))
            out.setdefault(cur, {"vs": int(m.group(3), 16), "c": {}})
        if cur is None:
            continue
        for idx, body in VEC_RE.findall(line):
            parts = [p.strip() for p in body.split(",")]
            if len(parts) != 4:
                continue
            try:
                out[cur]["c"][int(idx)] = tuple(float(p) for p in parts)
            except ValueError:
                pass  # -nan and friends: left out, and reported as missing
    return out


def mul_row(c, i, sw):
    """c[i] read through a channel swizzle like 'wyxz'."""
    v = c[i]
    return tuple(v["xyzw".index(ch)] for ch in sw)


def world_of(c, lay):
    """Return the registered world-transform columns.

    Their validated composition is a conventional matrix even though the source
    constant order differs. An earlier literal channel rotation failed the
    rasterized-draw calibration, which is why callers must retain that gate.
    """
    w0, w1, w2, w3 = lay["world"]
    return dict(x=c[w0], z=c[w1], y=c[w2], t=c[w3])


def bone_origin(c, lay, b):
    """Return where bone b maps the origin in object space.

    The registered interface stores three rows per bone. Their translation
    components are the final element of each row.
    """
    base = lay["bones_at"] + 3 * b
    if not all(base + k in c for k in (0, 1, 2)):
        return None
    # AN UNUSED PALETTE SLOT IS NOT A JOINT. The shader declares 256 float
    # constants, so the palette has room for 82 bones, and a character uses
    # about 45 of them -- the rest are left all-zero. A zero matrix maps
    # (0,0,0,1) to the origin, which the world transform then places at c3,
    # a single point that may well be on screen. Counting those as joints put
    # 9 phantom joints on screen for a draw whose real skeleton has none there,
    # which is a wrong answer with a plausible number attached.
    if all(all(v == 0.0 for v in c[base + k]) for k in (0, 1, 2)):
        return "unused"
    # The row-to-component order is calibrated empirically. Candidate orders
    # were checked against a draw known to rasterize, and only this ordering put
    # a majority of observed joints inside a compact screen-space extent.
    return (c[base + 0][3], c[base + 1][3], c[base + 2][3])


def to_clip(p, c, lay):
    w = world_of(c, lay)
    wp = tuple(p[0] * w["x"][i] + p[1] * w["y"][i] + p[2] * w["z"][i] + w["t"][i]
               for i in range(4))
    # The registered view-projection is a conventional four-column transform.
    # Carry the world-space w component rather than assuming it is one.
    v0, v1, v2, v3 = lay["viewproj"]
    cl = tuple(wp[0] * c[v0][i] + wp[1] * c[v1][i] + wp[2] * c[v2][i]
               + wp[3] * c[v3][i] for i in range(4))
    return wp, cl


def classify(cl):
    if cl[3] <= 0:
        return "behind"
    ndc = tuple(cl[i] / cl[3] for i in range(3))
    if all(abs(ndc[i]) <= 1 for i in range(2)) and 0 <= ndc[2] <= 1:
        return "on-screen"
    return "off-screen"


def analyse(diag, e):
    lay = KNOWN.get(e["vs"])
    if lay is None:
        print(f"draw {diag}: REFUSED -- vs 0x{e['vs']:016x} has no registered "
              f"constant layout. Only {len(KNOWN)} shader profile(s) are known; "
              f"guessing would produce confident wrong coordinates. Nothing computed.")
        return None
    c = e["c"]
    origins, unused, n = [], 0, 0
    while True:
        p = bone_origin(c, lay, n)
        if p is None:
            break
        n += 1
        if p == "unused":
            unused += 1
            continue
        origins.append(p)
    if not origins:
        print(f"draw {diag}: NO BONE ROWS in this log from c{lay['bones_at']} "
              f"onward -- nothing computed. Widen GEARS_DRAW_VS_CONSTS.")
        return None
    need = set(lay["world"]) | set(lay["viewproj"])
    missing = sorted(k for k in need if k not in c)
    if missing:
        print(f"draw {diag}: MISSING constants {missing} (the world matrix "
              f"and/or the view-projection) -- nothing computed.")
        return None
    counts = {"on-screen": 0, "off-screen": 0, "behind": 0}
    worst = []
    for p in origins:
        wp, cl = to_clip(p, c, lay)
        k = classify(cl)
        counts[k] += 1
        if k != "behind":
            worst.append(tuple(cl[i] / cl[3] for i in range(2)))
    print(f"draw {diag}: vs 0x{e['vs']:016x}, {len(origins)} bone joints "
          f"({unused} of {n} palette slots are all-zero and were skipped as "
          f"unused, not counted as joints at the origin)")
    print(f"   on-screen {counts['on-screen']}, off-screen "
          f"{counts['off-screen']}, behind the camera {counts['behind']}")
    if worst:
        xs = [w[0] for w in worst]
        ys = [w[1] for w in worst]
        print(f"   ndc.x {min(xs):+.2f} .. {max(xs):+.2f}   "
              f"ndc.y {min(ys):+.2f} .. {max(ys):+.2f}   (on screen is -1..+1)")
    return counts


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--calibrate", default="",
                    help="a log whose draw the renderer SHADED; its skeleton "
                         "must land on screen or nothing is reported")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.logs:
        ap.error("give at least one log, or --selftest")

    if not a.calibrate:
        print("UNCALIBRATED: no --calibrate log given, so the constant layout "
              "and its swizzles were not checked against a draw whose answer is "
              "known. A swizzle read wrong here produces coordinates, not an "
              "error. Nothing computed.")
        return 2
    cal = parse(Path(a.calibrate).read_text())
    print("-- calibration --")
    ok = False
    for diag, e in sorted(cal.items()):
        r = analyse(diag, e)
        # A MAJORITY, not "at least one". The first version of this gate asked
        # only for a single on-screen joint, and a layout that was actually
        # wrong sailed through it with 1 of 45 while scattering the rest across
        # 345 screens. A draw the GPU rasterised has most of its skeleton in
        # frame, so that is what is required.
        if r and r["on-screen"] * 2 > sum(r.values()):
            ok = True
    if not ok:
        print("   FAILED: the calibration draw's skeleton does not land on")
        print("   screen, though the renderer rasterised it. The layout or a")
        print("   swizzle is wrong and NO verdict is printed.")
        return 1
    print("   OK: the calibration draw rasterised on the GPU and its skeleton")
    print("   lands on screen, so the arithmetic agrees with the hardware on a")
    print("   case whose answer is known.\n")

    status = 0
    for log in a.logs:
        print(f"-- {log} --")
        draws = parse(Path(log).read_text())
        if not draws:
            print(f"   REFUSING: {log} carries no VS_CONSTS lines at all. This "
                  f"is an empty log, not a character that is off screen.")
            status = 2
            continue
        for diag, e in sorted(draws.items()):
            analyse(diag, e)
    return status


def selftest():
    """Both classes, on synthetic constants with an arithmetic answer.

    A camera at the origin looking down +x with a trivial projection: a bone in
    front must read on-screen, one behind must read behind, and an unknown
    shader hash must be REFUSED rather than computed.
    """
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
        ok = ok and cond

    def consts(bone_translation):
        c = {}
        # Identity world transform expressed in the registered column order.
        c[0] = (1.0, 0.0, 0.0, 0.0)   # x column
        c[1] = (0.0, 0.0, 1.0, 0.0)   # z column
        c[2] = (0.0, 1.0, 0.0, 0.0)   # y column
        c[3] = (0.0, 0.0, 0.0, 1.0)   # translation, w = 1
        # view-projection: x_ndc = y_world, y_ndc = z_world, w = x_world, so a
        # point at x>0 is in front and x<0 is behind.
        c[233] = (0.0, 0.0, 0.0, 1.0)  # x_world becomes clip w
        c[234] = (1.0, 0.0, 0.0, 0.0)  # y_world becomes ndc.x
        c[235] = (0.0, 1.0, 0.0, 0.0)  # z_world becomes ndc.y
        c[236] = (0.0, 0.0, 0.0, 0.0)
        # Bone 0 translation in the registered three-row layout.
        c[8] = (0.0, 0.0, 0.0, bone_translation[0])
        c[9] = (0.0, 0.0, 0.0, bone_translation[2])
        c[10] = (0.0, 0.0, 0.0, bone_translation[1])
        return c

    front = {"vs": 0x15CBC482459FE5B7, "c": consts((10.0, 0.0, 0.0))}
    behind = {"vs": 0x15CBC482459FE5B7, "c": consts((-10.0, 0.0, 0.0))}
    lay = KNOWN[0x15CBC482459FE5B7]

    check("a bone's origin is recovered from the three rows' .w",
          bone_origin(front["c"], lay, 0) == (10.0, 0.0, 0.0),
          str(bone_origin(front["c"], lay, 0)))
    _, cl_f = to_clip((10.0, 0.0, 0.0), front["c"], lay)
    _, cl_b = to_clip((-10.0, 0.0, 0.0), behind["c"], lay)
    check("a joint in front of the camera has w > 0", cl_f[3] > 0, f"w={cl_f[3]}")
    check("a joint behind the camera has w < 0", cl_b[3] < 0, f"w={cl_b[3]}")
    check("classify calls the front one on-screen",
          classify(cl_f) == "on-screen", classify(cl_f))
    check("classify calls the back one behind", classify(cl_b) == "behind",
          classify(cl_b))
    # NEGATIVE: an unknown shader must be refused, not computed.
    unknown = {"vs": 0xDEADBEEF, "c": consts((10.0, 0.0, 0.0))}
    check("an unknown shader hash is REFUSED", analyse(0, unknown) is None)
    # NEGATIVE: a log with no constants must parse to nothing.
    check("a log with no VS_CONSTS parses to nothing", parse("nothing") == {})
    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
