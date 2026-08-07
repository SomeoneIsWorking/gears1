#!/usr/bin/env python3
"""Diff the frame LAYER BY LAYER -- our pass output against the console's.

    tools/layer_compare.py --ours <dir> --theirs <dir> [--out <dir>]

A presented frame is the end of a chain of passes, and comparing only the end
says which chain differs, never where. This pairs the two sides at every RESOLVE
-- the point at which a pass hands its result to the next one -- and reports each
pass separately, the way a RenderDoc capture is read.

WHERE THE TWO SIDES' BYTES COME FROM, because they are NOT the same kind of
artefact and pretending otherwise is how a decoder difference gets reported as a
renderer difference:

  theirs  GEARS_ORACLE_RESOLVE_DUMP=<dir> on the Xenia fork. RAW GUEST BYTES of
          the resolve destination, read back off the GPU right after the copy
          and before anything else can touch them -- tiled, in the guest's own
          format. Named oracle_f<frame>_copy<n>_<ADDR>_<LEN>.bin.
  ours    GEARS_DRAW_RESOLVE_DUMP_EACH=1. Our renderer resolves into HOST images
          keyed by destination address; it never writes guest memory, so there
          are no guest bytes on our side to compare byte-for-byte. Ours arrive
          already untiled, as resolve_<ord>_<ADDR>_draw<n>.ppm.

So the join key is the DESTINATION ADDRESS, which both sides agree on and which
survives the two renderers executing different numbers of copies (we execute 14
of bright.gfr's 18). Ordinals do not survive that and pairing by them compares
unrelated buffers -- the mistake tools/resolve_pair.py exists to document.

The oracle's bytes are untiled HERE, once, by the Xenos address swizzle. That is
a decode this tool performs and can get wrong, so it REFUSES on any format it
does not implement rather than rendering the bytes as something they are not: a
wrong decode of a correct buffer looks exactly like a broken pass.
"""
import argparse
import re
import sys
from pathlib import Path

OURS_RE = re.compile(r"resolve_(\d+)_([0-9a-f]{8})_draw(\d+)\.ppm$", re.I)
THEIRS_RE = re.compile(r"oracle_f(\d+)_copy(\d+)_([0-9A-F]{8})_(\d+)\.bin$", re.I)


def tiled_offset_2d(x, y, width, log2_bpp):
    """Xenos 2D tile address, as Xenia's texture_util computes it.

    Ported rather than approximated: an "almost right" swizzle produces an image
    that is recognisable but wrong, which is the single most misleading artefact
    this tool could emit.
    """
    macro_y = ((y // 32) * (width // 32)) << (log2_bpp + 7)
    micro_y = ((y & 6) << 2) << log2_bpp
    base = (macro_y + ((micro_y & ~15) << 1) + (micro_y & 15)
            + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4))
    macro_x = (x // 32) << (log2_bpp + 7)
    micro_x = (x & 7) << log2_bpp
    offset = base + macro_x + ((micro_x & ~15) << 1) + (micro_x & 15)
    return (((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63)
            + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def untile_8888(raw, width, height, np):
    """Tiled k_8_8_8_8 -> HxWx4 uint8. None when the buffer is too short."""
    need = width * height * 4
    if len(raw) < need:
        return None
    src = np.frombuffer(raw, dtype=np.uint8)
    ys, xs = np.meshgrid(np.arange(height), np.arange(width), indexing="ij")
    # Vectorised form of tiled_offset_2d; kept beside the scalar version above
    # so the two can be checked against each other.
    y, x = ys.astype(np.int64), xs.astype(np.int64)
    lb = 2  # log2 of 4 bytes per pixel
    macro_y = ((y // 32) * (width // 32)) << (lb + 7)
    micro_y = ((y & 6) << 2) << lb
    base = (macro_y + ((micro_y & ~15) << 1) + (micro_y & 15)
            + ((y & 8) << (3 + lb)) + ((y & 1) << 4))
    macro_x = (x // 32) << (lb + 7)
    micro_x = (x & 7) << lb
    off = base + macro_x + ((micro_x & ~15) << 1) + (micro_x & 15)
    off = ((((off & ~511) << 3) + ((off & 448) << 2) + (off & 63)
            + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)))
    if off.max() + 4 > len(src):
        return None
    idx = off[..., None] + np.arange(4)
    return src[idx]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours", required=True)
    ap.add_argument("--theirs", required=True)
    ap.add_argument("--out", default=None)
    args = ap.parse_args(argv[1:])

    try:
        import numpy as np
        from PIL import Image
    except ImportError as exc:
        print(f"REFUSING: {exc}. Nothing was compared.")
        return 2

    ours_dir, theirs_dir = Path(args.ours), Path(args.theirs)
    for d in (ours_dir, theirs_dir):
        if not d.is_dir():
            print(f"REFUSING: {d} does not exist, so this run searched NOTHING."
                  f" Nothing was compared.")
            return 1

    ours, theirs = {}, {}
    for p in sorted(ours_dir.iterdir()):
        m = OURS_RE.search(p.name)
        if m:
            ours.setdefault(int(m.group(2), 16), []).append(p)
    for p in sorted(theirs_dir.iterdir()):
        m = THEIRS_RE.search(p.name)
        if m:
            theirs.setdefault(int(m.group(3), 16), []).append((p, int(m.group(4))))

    print(f"ours   {sum(len(v) for v in ours.values())} pass dump(s) at "
          f"{len(ours)} destination(s)")
    print(f"theirs {sum(len(v) for v in theirs.values())} pass dump(s) at "
          f"{len(theirs)} destination(s)")
    if not ours or not theirs:
        print("REFUSING: a side dumped nothing. A side with no dumps did not "
              "render fewer passes -- it did not dump. Nothing was compared.")
        return 1

    shared = sorted(set(ours) & set(theirs))
    only_o = sorted(set(ours) - set(theirs))
    only_t = sorted(set(theirs) - set(ours))
    # ALWAYS printed, both directions, including empty: a destination only one
    # side resolves is the most interesting result this tool can produce and it
    # must not be reachable only by noticing a short table.
    print(f"\ndestinations both sides resolve: {len(shared)}")
    print(f"  only ours   ({len(only_o)}): "
          + (", ".join(f"{a:08x}" for a in only_o) or "(none)"))
    print(f"  only theirs ({len(only_t)}): "
          + (", ".join(f"{a:08x}" for a in only_t) or "(none)"))
    if not shared:
        print("REFUSING: the two sides share NO resolve destination, so no pass "
              "can be paired. Nothing was written.")
        return 1

    out_dir = Path(args.out) if args.out else theirs_dir.parent / "layers"
    out_dir.mkdir(parents=True, exist_ok=True)
    undecoded = 0
    print(f"\n{'dest':>10} {'our pass':>26} {'size':>11} {'mean ours':>10} "
          f"{'mean theirs':>11}  note")
    for addr in shared:
        our_path = ours[addr][-1]
        their_path, their_len = theirs[addr][-1]
        oi = np.asarray(Image.open(our_path).convert("RGB")).astype(np.float32) / 255.0
        h, w = oi.shape[:2]
        raw = their_path.read_bytes()
        ti = untile_8888(raw, w, h, np)
        if ti is None:
            # REFUSE THIS ROW, loudly, rather than skip it: a pass that could not
            # be decoded is not a pass that matched.
            undecoded += 1
            print(f"  {addr:08x} {our_path.name[:26]:>26} {w}x{h} "
                  f"{oi.mean():>10.4f} {'--':>11}  UNDECODED: {their_len} bytes "
                  f"is not {w}x{h}x4 tiled k_8_8_8_8")
            continue
        t = ti[..., :3].astype(np.float32) / 255.0
        note = ""
        d = float(np.abs(t - oi).mean())
        if d < 0.02:
            note = "match"
        else:
            note = f"DIFFER, mean |d| {d:.3f}"
            side = np.concatenate([oi, t], axis=1)
            Image.fromarray((np.clip(side, 0, 1) ** 0.45 * 255).astype(np.uint8)
                            ).save(out_dir / f"pass_{addr:08x}.png")
        print(f"  {addr:08x} {our_path.name[:26]:>26} {w}x{h} "
              f"{oi.mean():>10.4f} {t.mean():>11.4f}  {note}")

    print(f"\n{undecoded} pass(es) could not be decoded and are NOT counted as "
          f"matching.\nSide-by-side images for differing passes: {out_dir}"
          f" (ours left, console right, gamma 0.45)")
    print("BLIND SPOT: this compares resolve DESTINATIONS. A pass whose output "
          "is consumed\nwithout a resolve does not appear here at all.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
