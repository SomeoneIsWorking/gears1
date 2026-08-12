#!/usr/bin/env python3
"""Which draws MARK stencil, which draws CONSUME it, and do the counts compose?

    tools/mask_ledger.py --diag <draws.tsv> [--stencil <dir with *.npy>]
    tools/mask_ledger.py --selftest

WHY THIS AND NOT A CORRELATION. A stencil-gated pass is a chain of promises:
a marking draw writes stencil, and the shading draw after it renders exactly
where the mark landed. That is checkable ARITHMETIC on one side -- no console,
no pairing, no camera gate, no threshold -- and it localises to a single draw.
Catalog #91 spent days scoring resolved images against the console while the
answer was that one marking draw wrote nothing and the two draws after it
therefore shaded nothing.

WHAT THE ROWS MEAN, and the two ways this table has been misread:

  * A MARKING DRAW'S OWN FRAGMENT COUNT IS MEANINGLESS. It renders with the
    colour mask off, so a driver may skip the fragment shader entirely and
    report zero invocations for a draw that wrote stencil across the screen.
    Only the SHADING draws' counts are evidence. Marks are reported with their
    stencil sample count when a dump is supplied, and with "not measured"
    otherwise -- never with their fragment count.
  * THE STENCIL OPS ARE DECODED FROM RB_DEPTHCONTROL, NOT FROM THE NAMED
    COLUMNS. The table's stencil_fail/zpass/zfail headers were once emitted in
    the wrong order and sent this issue chasing a bug that did not exist, and
    old draws.tsv files on disk still carry the wrong order. depth_control is
    the register itself and cannot be mislabelled, so every op here comes from
    bits 11/14/17 (front) and 23/26/29 (back) of that value. backface_enable is
    bit 7 and is ALWAYS reported: a depth-fail volume is DEFINED by the two
    faces disagreeing, and a reading that ignores the back face concluded that
    a stencil value the hardware produces routinely was impossible.
"""
import argparse
import csv
import pathlib
import re
import sys

OPS = {0: "KEEP", 1: "ZERO", 2: "REPLACE", 3: "INCR_CLAMP", 4: "DECR_CLAMP",
       5: "INVERT", 6: "INCR_WRAP", 7: "DECR_WRAP"}
CMP = {0: "NEVER", 1: "LESS", 2: "EQUAL", 3: "LEQUAL", 4: "GREATER",
       5: "NOTEQUAL", 6: "GEQUAL", 7: "ALWAYS"}
# An op that can CHANGE the stencil. KEEP never does, so a draw whose three
# front ops are all KEEP is a consumer however its colour mask reads.
WRITES = {1, 2, 3, 4, 5, 6, 7}


def as_int(v, default=0):
    if v is None or v == "":
        return default
    try:
        return int(v, 16) if str(v).startswith("0x") else int(v)
    except ValueError:
        return default


def decode(dc):
    return {
        "on": dc & 1,
        "zfunc": CMP[(dc >> 4) & 7],
        "zwrite": (dc >> 2) & 1,
        "func": CMP[(dc >> 8) & 7],
        "fail": (dc >> 11) & 7,
        "zpass": (dc >> 14) & 7,
        "zfail": (dc >> 17) & 7,
        "bf": (dc >> 7) & 1,
        "bf_fail": (dc >> 23) & 7,
        "bf_zpass": (dc >> 26) & 7,
        "bf_zfail": (dc >> 29) & 7,
    }


def stencil_counts(d):
    """Marked-sample counts per diag index, from GEARS_DRAW_DEPTH_DUMP_PS."""
    import numpy as np
    out = {}
    for f in sorted(pathlib.Path(d).glob("depth_after_diag*.npy")):
        m = re.search(r"depth_after_diag(\d+)\.npy", f.name)
        if not m:
            continue
        a = np.load(str(f))
        # channel 0 is depth, channel 1 the raw stencil byte
        s = a[..., 1] if a.ndim == 3 and a.shape[-1] > 1 else a
        vals, cnt = np.unique(s, return_counts=True)
        out[int(m.group(1))] = {int(v): int(c) for v, c in zip(vals, cnt)}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--diag", help="a GEARS_DRAW_DIAG draws.tsv")
    ap.add_argument("--stencil", help="directory of depth_after_diag*.npy")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.diag:
        raise SystemExit("REFUSING: --diag is required. NOTHING was read.")
    p = pathlib.Path(a.diag)
    if not p.is_file():
        raise SystemExit(f"REFUSING: {p} does not exist. NOTHING was read -- "
                         f"this is not a frame with no stencil draws.")
    rows = list(csv.DictReader(p.open(), delimiter="\t"))
    if not rows:
        raise SystemExit(f"REFUSING: {p} has no rows. NOTHING was read.")
    if "depth_control" not in rows[0]:
        raise SystemExit(
            f"REFUSING: {p} has no depth_control column, so the stencil ops "
            f"could only be read from the NAMED columns -- and those were "
            f"emitted in the wrong order by older builds, which is how this "
            f"issue acquired a bug that did not exist. Re-capture with a "
            f"current build. NOTHING was decoded.")

    marks = shades = 0
    entries = []
    for x in rows:
        dc = as_int(x.get("depth_control"))
        if not (dc & 1):
            continue
        d = decode(dc)
        writes = (d["fail"] in WRITES or d["zpass"] in WRITES
                  or d["zfail"] in WRITES
                  or (d["bf"] and (d["bf_fail"] in WRITES
                                   or d["bf_zpass"] in WRITES
                                   or d["bf_zfail"] in WRITES)))
        gated = d["func"] not in ("ALWAYS", "NEVER")
        entries.append((as_int(x.get("draw")), x, d, writes, gated))
        marks += 1 if (writes and not gated) else 0
        shades += 1 if gated else 0

    print(f"{p}: {len(rows)} draw(s), {len(entries)} with the stencil test "
          f"enabled -- {marks} that can WRITE stencil, {shades} whose "
          f"rendering is GATED on it.")
    if not entries:
        print("NO DRAW IN THIS FRAME ENABLES THE STENCIL TEST. That is a "
              "complete answer about this frame, not an empty result: the "
              "denominator above is the whole draw list.", file=sys.stderr)
        return 1

    sten = {}
    if a.stencil:
        sd = pathlib.Path(a.stencil)
        if not sd.is_dir():
            raise SystemExit(f"REFUSING: --stencil {sd} is not a directory.")
        sten = stencil_counts(sd)
        print(f"stencil dumps for diag index(es): "
              f"{sorted(sten) if sten else 'NONE FOUND in ' + str(sd)}")
    else:
        print("no --stencil given, so what each MARK actually wrote is NOT "
              "measured here; marking draws are listed without a count rather "
              "than with their (meaningless) fragment count")

    print(f"\n{'draw':>6} {'role':>6} {'prims':>7} {'frags':>10} "
          f"{'marked':>12}   state")
    pending, composed, broken = None, [], []
    for draw, x, d, writes, gated in entries:
        prims = x.get("ia_prims", "")
        frags = as_int(x.get("frag_invocations"), -1)
        # A DRAW CAN BOTH CONSUME AND WRITE, and calling it a producer breaks
        # the pairing. This title's mask shading draws test NOTEQUAL 0 AND
        # write REPLACE on zpass -- they are consumers that also update the
        # stencil as they go. The COMPARE FUNCTION decides the role: ALWAYS or
        # NEVER cannot gate anything, so such a draw is a pure producer;
        # anything else is gated and is a consumer whatever else it writes.
        if writes and not gated:
            got = sten.get(draw)
            nonzero = (sum(c for v, c in got.items() if v != 0)
                       if got is not None else None)
            mk = ("not measured" if nonzero is None else f"{nonzero:,}")
            ops = (f"func={d['func']} ref={as_int(x.get('stencil_ref'))} "
                   f"fail={OPS[d['fail']]} zpass={OPS[d['zpass']]} "
                   f"zfail={OPS[d['zfail']]}")
            if d["bf"]:
                ops += (f" | BACKFACE ON bf_zfail={OPS[d['bf_zfail']]} "
                        f"bf_zpass={OPS[d['bf_zpass']]}")
                if (d["zfail"], d["bf_zfail"]) == (6, 7):
                    ops += "  [depth-fail volume, Carmack's reverse]"
            print(f"{draw:>6} {'MARK':>6} {prims:>7} {'--':>10} {mk:>12}   "
                  f"{ops}  zfunc={d['zfunc']} zwrite={d['zwrite']}")
            pending = (draw, nonzero)
        else:
            also = ""
            if writes:
                also = (f"  [also WRITES: zpass={OPS[d['zpass']]} "
                        f"zfail={OPS[d['zfail']]}]")
            print(f"{draw:>6} {'shade':>6} {prims:>7} {frags:>10} "
                  f"{'':>12}   func={d['func']} "
                  f"ref={as_int(x.get('stencil_ref'))} zfunc={d['zfunc']}{also}")
            # DOES THE CONSUMER TAKE WHAT THE PRODUCER WROTE? Only meaningful
            # when the mark's stencil was dumped AND the consumer is asking for
            # the marked pixels.
            #
            # A MARK ONLY ANSWERS FOR ITS OWN PASS. Left unbounded, one mark was
            # paired with every gated draw in the rest of the frame and reported
            # eight "mismatches" against a later pass that has nothing to do with
            # it. Two rules bound it: a consumer testing NOTEQUAL 0 is asking
            # "where was I marked" and its count is directly comparable, while a
            # consumer testing EQUAL 0 is asking for the COMPLEMENT and its count
            # is not; and the first consumer of any other kind ends the pass, so
            # the pending mark is dropped rather than carried into it.
            if not (d["func"] == "NOTEQUAL"
                    and as_int(x.get("stencil_ref")) == 0):
                pending = None
                continue
            if pending and pending[1] is not None:
                mdraw, mcount = pending
                # 0 -> 0 is arithmetically consistent and is NOT a success:
                # it is the pass doing nothing. Routed to the dead-pair report
                # so the frame's most important row cannot read as a pass.
                if mcount == 0 and frags == 0:
                    broken.append((mdraw, draw, 0, 0))
                elif frags == mcount:
                    composed.append((mdraw, draw, mcount))
                else:
                    broken.append((mdraw, draw, mcount, frags))

    print()
    if not sten:
        print("NO MARK/CONSUME ARITHMETIC WAS DONE, because no stencil dump "
              "was supplied. Re-run with GEARS_DRAW_DEPTH_DUMP_PS=<ps hash>"
              ",marked and pass --stencil. The table above is state only.")
        return 0
    for m, s, c in composed:
        print(f"COMPOSES: draw {m} marked {c:,} samples and draw {s} shaded "
              f"exactly {c:,} fragments.")
    for m, s, c, fr in broken:
        if c == 0 and fr == 0:
            print(f"DEAD PAIR: draw {m} marked NOTHING and draw {s} therefore "
                  f"shaded nothing. The defect is in the MARK, and everything "
                  f"downstream of it is unattributable until it is fixed. Note "
                  f"a depth-fail volume marking zero can be CORRECT -- its two "
                  f"ops cancel except where geometry lies inside the volume -- "
                  f"so this names the draw to investigate, not a bug.")
        else:
            print(f"MISMATCH: draw {m} marked {c:,} samples but draw {s} shaded "
                  f"{fr:,} fragments. A consumer that does not take what its "
                  f"producer wrote is a defect between them -- a depth test, a "
                  f"scissor, or geometry that does not cover the marks.")
    if not composed and not broken:
        print("NO MARK WAS FOLLOWED BY A GATED DRAW whose stencil was dumped, "
              "so nothing was checked. Aim the dump at the marking shader.")
    return 0


def selftest():
    """Drive all three outcomes on synthetic rows: compose, dead, mismatch."""
    import io
    # depth_control: stencil on, ALWAYS, zpass REPLACE -> a mark
    mark = 1 | (7 << 8) | (2 << 14)
    # stencil on, NOTEQUAL -> a consumer, all ops KEEP
    shade = 1 | (5 << 8)
    hdr = "draw\tdepth_control\tia_prims\tfrag_invocations\tstencil_ref\n"
    body = (f"1\t{hex(mark)}\t10\t0\t1\n2\t{hex(shade)}\t10\t500\t0\n"
            f"3\t{hex(mark)}\t10\t0\t1\n4\t{hex(shade)}\t10\t0\t0\n"
            f"5\t{hex(mark)}\t10\t0\t1\n6\t{hex(shade)}\t10\t77\t0\n")
    rows = list(csv.DictReader(io.StringIO(hdr + body), delimiter="\t"))
    ok = True
    for x, want_writes in zip(rows, [True, False] * 3):
        d = decode(as_int(x["depth_control"]))
        writes = d["fail"] in WRITES or d["zpass"] in WRITES or d["zfail"] in WRITES
        if writes != want_writes:
            ok = False
        print(f"  draw {x['draw']}: writes={writes} (want {want_writes}) "
              f"func={d['func']}")
    dc = 0xe07c07e3
    d = decode(dc)
    volume = d["bf"] == 1 and d["zfail"] == 6 and d["bf_zfail"] == 7
    print(f"REAL REGISTER 0xe07c07e3 decodes as a depth-fail volume "
          f"(backface_enable={d['bf']}, front zfail={OPS[d['zfail']]}, back "
          f"zfail={OPS[d['bf_zfail']]}): {'PASS' if volume else 'FAIL'}")
    dc2 = 0x00708763
    d2 = decode(dc2)
    zpass_mark = d2["bf"] == 0 and d2["zpass"] == 2 and d2["zfail"] == 0
    print(f"REAL REGISTER 0x00708763 decodes as a ZPASS-REPLACE mark with no "
          f"back face: {'PASS' if zpass_mark else 'FAIL'}")
    print("  both are the registers of the two marking draws this tool was "
          "written for, so the decode is checked against real values and not "
          "only synthetic ones -- and they must decode DIFFERENTLY, which is "
          "the distinction a previous reading of this pass got wrong")
    ok = ok and volume and zpass_mark and (dc != dc2)
    print(f"selftest: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
