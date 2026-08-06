#!/usr/bin/env python3
"""Did the GUEST put this draw outside the frustum, or did we lose it?

`tools/clip_volume_check.py` answered that once, for three draws of one capture,
with every number inlined. This does it for any draw of any capture, because the
question keeps coming back: 276 of walk_gameplay.gfr's draws report
`killed_by_clip_or_cull`, and "our clip is broken" and "UE3 submitted geometry
that is genuinely off-camera" look identical in the diag table.

    GEARS_DRAW_VDUMP=292,293,294 GEARS_DRAW_VS_CONSTS=292,293,294 \
        frame_replay <capture> > run.log
    tools/clip_check.py run.log                 # every draw the log carries
    tools/clip_check.py run.log --draws 293,294
    tools/clip_check.py --selftest

THE LAYOUT IS A HYPOTHESIS, and the run says so. c0,c1,c2 are taken as the world
matrix' columns with c3 its translation, and c7,c8,c9 as the view-projection's
columns with c10 its translation. That is what `clip_volume_check.py` established
against a draw the GPU demonstrably rasterised, and it is re-checked here every
run: pass a draw that DID rasterise and this must place it inside, or the answer
for the killed draws is worthless. A run whose log contains no such draw says it
is UNCALIBRATED rather than printing verdicts as if they were settled.

What it cannot do: it reads the FIRST FEW dumped vertices, not the whole buffer,
so "all dumped vertices outside" is evidence and not proof for a large mesh --
the count is printed for that reason. And it says nothing about why a draw with
primitives left after clipping still shows nothing.
"""
import argparse
import re
import sys
from pathlib import Path

VERT_RE = re.compile(
    r"draw (\d+) vertex (\d+) @ 0x([0-9a-f]+) \(stride (\d+) dwords\): (.*)")
VAL_RE = re.compile(r"\[(\d+)\]0x[0-9a-f]+=(-?[0-9.eE+-]+)")
CONST_RE = re.compile(r"draw (\d+) \(diag \d+\) vs 0x[0-9a-f]+ float constants")
VEC_RE = re.compile(r"c\[(\d+)\]=\(([^)]*)\)")


def parse(log_text):
    """-> {draw: {'verts': [(x,y,z), ...], 'c': {i: (x,y,z,w)}}}"""
    draws = {}
    for line in log_text.splitlines():
        m = VERT_RE.search(line)
        if m:
            d, _, _, stride, rest = m.groups()
            if int(stride) < 3:
                continue
            vals = {int(i): float(v) for i, v in VAL_RE.findall(rest)}
            if not all(k in vals for k in (0, 1, 2)):
                continue
            e = draws.setdefault(int(d), {"verts": [], "c": {}})
            # The POSITION stream only. A draw fetches several streams and the
            # later ones (per-vertex lighting, stride 3) are not coordinates;
            # taking them as positions is how this reads as "everything is at
            # the origin". Keyed on the widest stride seen for the draw.
            e.setdefault("stride", int(stride))
            if int(stride) >= e["stride"]:
                if int(stride) > e["stride"]:
                    e["verts"] = []
                    e["stride"] = int(stride)
                e["verts"].append((vals[0], vals[1], vals[2]))
        m = CONST_RE.search(line)
        if m:
            d = int(m.group(1))
            e = draws.setdefault(d, {"verts": [], "c": {}})
            for idx, body in VEC_RE.findall(line):
                parts = [p.strip() for p in body.split(",")]
                if len(parts) == 4:
                    try:
                        e["c"][int(idx)] = tuple(float(p) for p in parts)
                    except ValueError:
                        pass
    return draws


def world(v, c):
    return tuple(v[0] * c[0][i] + v[1] * c[1][i] + v[2] * c[2][i] + c[3][i]
                 for i in range(3))


def clip(w, c):
    return tuple(w[0] * c[7][i] + w[1] * c[8][i] + w[2] * c[9][i] + c[10][i]
                 for i in range(4))


def inside(cl):
    return cl[3] > 0 and all(abs(cl[i]) <= cl[3] for i in range(3))


def report(d, e, verdicts):
    need = (0, 1, 2, 3, 7, 8, 9, 10)
    if not all(k in e["c"] for k in need):
        print(f"draw {d}: NO CONSTANTS in this log (need c0..c3, c7..c10) -- "
              f"nothing computed. Add it to GEARS_DRAW_VS_CONSTS.")
        return None
    if not e["verts"]:
        print(f"draw {d}: NO VERTICES in this log -- nothing computed. "
              f"Add it to GEARS_DRAW_VDUMP.")
        return None
    n_in = 0
    print(f"draw {d}: renderer verdict {verdicts.get(d, '(not in a diag table)')}"
          f", {len(e['verts'])} dumped vertices")
    for v in e["verts"]:
        w = world(v, e["c"])
        cl = clip(w, e["c"])
        ok = inside(cl)
        n_in += ok
        where = ("BEHIND THE CAMERA (w<0)" if cl[3] <= 0 else
                 "ndc=(%+.3f, %+.3f, %+.3f)" % tuple(cl[i] / cl[3] for i in range(3)))
        print(f"   world=({w[0]:11.1f},{w[1]:11.1f},{w[2]:10.1f})  "
              f"w={cl[3]:11.1f}  {where}  {'inside' if ok else 'OUTSIDE'}")
    print(f"   -> {n_in} of {len(e['verts'])} dumped vertices inside the clip volume")
    return n_in


def load_verdicts(path):
    """draw -> verdict, from a GEARS_DRAW_DIAG table if one sits beside the log."""
    out = {}
    if not path or not Path(path).exists():
        return out
    with open(path) as f:
        head = f.readline().rstrip("\n").split("\t")
        if "draw" not in head or "verdict" not in head:
            return out
        di, vi = head.index("draw"), head.index("verdict")
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) > max(di, vi):
                out[int(p[di])] = p[vi]
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="a frame_replay log with VDUMP + VS_CONSTS")
    ap.add_argument("--draws", default="", help="comma-separated subset")
    ap.add_argument("--diag", default="", help="a GEARS_DRAW_DIAG tsv, for verdicts")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.log:
        ap.error("give a log, or --selftest")
    draws = parse(Path(a.log).read_text())
    if not draws:
        raise SystemExit(
            f"REFUSING: {a.log} contains no VDUMP/VS_CONSTS lines at all. This "
            f"is an EMPTY LOG, not a frame whose draws are all on-screen. Re-run "
            f"with GEARS_DRAW_VDUMP=<draws> GEARS_DRAW_VS_CONSTS=<draws>.")
    verdicts = load_verdicts(a.diag)
    want = [int(x) for x in a.draws.split(",") if x] or sorted(draws)
    missing = [d for d in want if d not in draws]
    if missing:
        print(f"NOT IN THE LOG: {missing} -- they were not dumped, which is not "
              f"the same as being on-screen.")
    results = {d: report(d, draws[d], verdicts) for d in want if d in draws}

    # CALIBRATION. The layout is a hypothesis; a run that cannot check it against
    # a draw the GPU kept must not read as a settled answer.
    print("\n-- calibration --")
    kept = [d for d in results
            if verdicts.get(d, "").startswith("shaded") and results[d] is not None]
    if not kept:
        print("   UNCALIBRATED: this run contains no draw the renderer says it")
        print("   SHADED, so the transform layout was not checked against a")
        print("   known-visible case. Dump one alongside and re-run before")
        print("   quoting any verdict above.")
        return 2
    bad = [d for d in kept if results[d] == 0]
    if bad:
        print(f"   FAILED: draw(s) {bad} rasterised on the GPU but this layout")
        print("   places every dumped vertex outside. The layout is wrong and")
        print("   every verdict above is unusable.")
        return 1
    print(f"   OK: draw(s) {kept} rasterised on the GPU and this layout places")
    print("   them inside, so the arithmetic agrees with the hardware on a case")
    print("   whose answer is known.")
    return 0


def selftest():
    """The three courtyard draws clip_volume_check.py established, inline.

    Keeps a known POSITIVE (288 rasterised 87 primitives) and known NEGATIVES
    (286, 287 produced none), plus a deliberately wrong layout that must be
    rejected -- a check that has only ever passed has not been shown to be one.
    """
    verts = [  # first four vertices of 0xe585000, stride 11, shared by all three
        (1169.7983, -1672.5906, 2168.2236),
        (1477.5428, -1969.5702, 1895.9917),
        (1278.5767, -2561.8410, 1212.4653),
        (1084.0103, -2590.8418, 1263.0806),
    ]
    vp = {7: (-1.1917515, 0.0, -0.0019157454, -0.001917663),
          8: (-0.0022853818, 0.0, 0.99899817, 0.99999815),
          9: (0.0, 2.118673, 0.0, 0.0),
          10: (123.781944, -557.5288, -535.76154, -526.29785)}
    cases = {
        286: dict(c0=(-2.0773509, -2.5312598, 0.0), c1=(2.5312598, -2.0773509, 0.0),
                  c2=(0.0, 0.0, 3.274548), c3=(15416.857, -2809.7197, -32.219723)),
        287: dict(c0=(0.32096183, -3.2587802, 0.0), c1=(3.2587802, 0.32096183, 0.0),
                  c2=(0.0, 0.0, 3.274548), c3=(26035.129, 10463.118, -3559.9998)),
        288: dict(c0=(0.32037368, 2.9828444, 0.0), c1=(-2.9828444, 0.32037368, 0.0),
                  c2=(0.0, 0.0, 3.0), c3=(-16564.238, 19323.756, -128.0)),
    }
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
        ok = ok and cond

    def n_inside(world_c, vpc):
        c = dict(vpc); c.update({i: world_c[f"c{i}"] + (0.0,) for i in range(4)
                                 if f"c{i}" in world_c})
        c[3] = world_c["c3"] + (1.0,)
        return sum(inside(clip(world(v, c), c)) for v in verts)

    check("288 rasterised on the GPU -> layout places it inside",
          n_inside(cases[288], vp) > 0, f"{n_inside(cases[288], vp)}/4 inside")
    for d in (286, 287):
        check(f"{d} produced no primitives -> layout places it outside",
              n_inside(cases[d], vp) == 0, f"{n_inside(cases[d], vp)}/4 inside")
    # NEGATIVE: a deliberately wrong view-projection must break the positive.
    wrong = {7: (1.0, 0, 0, 0), 8: (0, 1.0, 0, 0), 9: (0, 0, 1.0, 0),
             10: (0.0, 0.0, 0.0, 1.0)}
    check("a deliberately wrong view-projection FAILS the known-visible draw",
          n_inside(cases[288], wrong) == 0,
          f"{n_inside(cases[288], wrong)}/4 inside")
    # NEGATIVE: an empty log must refuse rather than report no draws clipped.
    check("an empty log parses to nothing", parse("no vertices here") == {})

    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
