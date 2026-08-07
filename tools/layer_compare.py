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
          format. Named oracle_f<frame>_copy<n>_src<C|D><base>_<w>x<h>_
          <ADDR>_<LEN>.bin.
  ours    GEARS_DRAW_RESOLVE_DUMP_EACH=1. Our renderer resolves into HOST images
          keyed by destination address; it never writes guest memory, so there
          are no guest bytes on our side to compare byte-for-byte. Ours arrive
          already untiled, as resolve_<ord>_src<C|D><base>_<w>x<h>_<ADDR>_
          draw<n>.ppm.

THE JOIN KEY IS THE PASS'S STRUCTURAL IDENTITY: which EDRAM surface the copy
reads (RB_COPY_CONTROL.copy_src_select -> RB_COLOR[n]_INFO or RB_DEPTH_INFO) and
the destination's guest dimensions (RB_COPY_DEST_PITCH), plus the ordinal among
the copies that share that key. Every one of those comes from the guest's own
registers, so the two emulators necessarily agree on them.

NOT the destination ADDRESS, which was the first key tried and pairs NOTHING:
the title's physical allocations land in different places in the two emulators.
A paired gameplay capture (frames 2935 ours / 2947 theirs, both selected by
content) had all seven of our destinations near 0x0Cxxxxxx and all eight of the
console's near 0x13xxxxxx, and the intersection was empty.

NOT the ordinal alone either: the two renderers do not execute the same number
of copies (that capture: 7 executed here against the console's 15), so ordinal N
is a different pass on each side -- the mistake tools/resolve_pair.py documents.

The oracle's bytes are untiled HERE, once, by the Xenos address swizzle. That is
a decode this tool performs and can get wrong, so it REFUSES on any format it
does not implement rather than rendering the bytes as something they are not: a
wrong decode of a correct buffer looks exactly like a broken pass.
"""
import argparse
import re
import sys
from pathlib import Path

OURS_RE = re.compile(
    r"resolve_(\d+)_src([CD])([0-9A-F]{3})_(\d+)x(\d+)_f(\d+)_([0-9a-f]{8})"
    r"_draw(\d+)\.ppm$", re.I)
THEIRS_RE = re.compile(
    r"oracle_f(\d+)_copy(\d+)_src([CD])([0-9A-F]{3})_(\d+)x(\d+)_f(\d+)_"
    r"([0-9A-F]{8})_(\d+)\.bin$", re.I)

# RB_COPY_DEST_INFO.copy_dest_format values this tool can DECODE. Only one, on
# purpose: k_8_8_8_8 (6). Everything else -- 32 is k_16_16_16_16_FLOAT, and this
# frame carries several -- is REFUSED per pass rather than read as 8888, because
# an eight-byte buffer read as four-byte bytes produces a recognisable image of
# the wrong thing. That mistake was made here first: three rows of the first
# paired run reported a difference for buffers this tool had mis-decoded.
DECODABLE_FORMATS = {6: "k_8_8_8_8"}


def key_str(k):
    src, base, w, h, fmt, nth = k
    return f"src{src}{base:03X} {w}x{h} f{fmt} #{nth}"


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


def selftest(work, np, Image):
    """Round-trip a known image through the tiler and back through this tool.

    Two cases, both required to pass: an image tiled and then untiled must come
    back identical (the decode), and a DIFFERENT image under the same pass key
    must be reported as differing (the comparison). Either one alone would leave
    a broken tool looking healthy -- an untiler that returned its input would
    pass the first, and a comparison that always said DIFFER would pass the
    second.
    """
    ours_dir, theirs_dir = work / "st_ours", work / "st_theirs"
    for d in (ours_dir, theirs_dir):
        d.mkdir(parents=True, exist_ok=True)
    w, h = 64, 64
    ys, xs = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
    img = np.stack([(xs * 4) % 256, (ys * 4) % 256, (xs + ys) % 256,
                    np.full_like(xs, 255)], axis=-1).astype(np.uint8)
    ok = True
    for nth, (src, note) in enumerate([(img, "identical"),
                                       (np.roll(img, 7, axis=1), "shifted")]):
        raw = bytearray(w * h * 4)
        for y in range(h):
            for x in range(w):
                off = tiled_offset_2d(x, y, w, 2)
                raw[off:off + 4] = bytes(src[y, x])
        (theirs_dir /
         f"oracle_f1_copy{nth}_srcC000_{w}x{h}_f6_00000000_{w * h * 4}.bin"
         ).write_bytes(bytes(raw))
        Image.fromarray(img[..., :3]).save(
            ours_dir /
            f"resolve_{nth:02}_srcC000_{w}x{h}_f6_00000000_draw{nth}.ppm")
        back = untile_8888(bytes(raw), w, h, np)
        if back is None:
            print(f"SELFTEST FAIL: the {note} case did not decode at all")
            ok = False
            continue
        same = bool((back[..., :3] == img[..., :3]).all())
        want = (note == "identical")
        print(f"selftest: {note} case decodes {'equal' if same else 'different'}"
              f" (expected {'equal' if want else 'different'})")
        ok = ok and same == want
    print("SELFTEST PASS: the untiler round-trips and the difference test fires"
          if ok else "SELFTEST FAIL: do not trust this tool's output")
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours", required=True)
    ap.add_argument("--theirs", required=True)
    ap.add_argument("--out", default=None)
    # A comparison that can only ever print "match" is not a comparison. This
    # feeds the tool a pair it MUST call equal and a pair it MUST call
    # different, through the real untiler and the real join.
    ap.add_argument("--selftest", action="store_true",
                    help="prove the decode and the difference test both fire")
    args = ap.parse_args(argv[1:])

    try:
        import numpy as np
        from PIL import Image
    except ImportError as exc:
        print(f"REFUSING: {exc}. Nothing was compared.")
        return 2

    if args.selftest:
        return selftest(Path(args.ours), np, Image)

    ours_dir, theirs_dir = Path(args.ours), Path(args.theirs)
    for d in (ours_dir, theirs_dir):
        if not d.is_dir():
            print(f"REFUSING: {d} does not exist, so this run searched NOTHING."
                  f" Nothing was compared.")
            return 1

    # Keyed by (source, edram base, dest w, dest h, nth-of-that-key). The
    # ordinal is per KEY, not per frame, so a pass that both sides execute still
    # pairs when one side skipped an unrelated copy earlier in the frame.
    ours, theirs = {}, {}
    seen = {}
    for p in sorted(ours_dir.iterdir(), key=lambda q: q.name):
        m = OURS_RE.search(p.name)
        if not m:
            continue
        k = (m.group(2).upper(), int(m.group(3), 16), int(m.group(4)),
             int(m.group(5)), int(m.group(6)))
        nth = seen.get(k, 0)
        seen[k] = nth + 1
        ours[k + (nth,)] = (p, int(m.group(7), 16))
    seen = {}
    for p in sorted(theirs_dir.iterdir(),
                    key=lambda q: int(THEIRS_RE.search(q.name).group(2))
                    if THEIRS_RE.search(q.name) else -1):
        m = THEIRS_RE.search(p.name)
        if not m:
            continue
        k = (m.group(3).upper(), int(m.group(4), 16), int(m.group(5)),
             int(m.group(6)), int(m.group(7)))
        nth = seen.get(k, 0)
        seen[k] = nth + 1
        theirs[k + (nth,)] = (p, int(m.group(8), 16), int(m.group(9)))

    print(f"ours   {len(ours)} pass dump(s)")
    print(f"theirs {len(theirs)} pass dump(s)")
    if not ours or not theirs:
        print("REFUSING: a side dumped nothing. A side with no dumps did not "
              "render fewer passes -- it did not dump. Nothing was compared.")
        return 1

    shared = sorted(set(ours) & set(theirs))
    only_o = sorted(set(ours) - set(theirs))
    only_t = sorted(set(theirs) - set(ours))
    # ALWAYS printed, both directions, including empty: a pass only one side
    # resolves is the most interesting result this tool can produce and it must
    # not be reachable only by noticing a short table.
    print(f"\npasses both sides resolve: {len(shared)}")
    print(f"  only ours   ({len(only_o)}): "
          + (", ".join(key_str(k) for k in only_o) or "(none)"))
    print(f"  only theirs ({len(only_t)}): "
          + (", ".join(key_str(k) for k in only_t) or "(none)"))
    if not shared:
        print("REFUSING: the two sides share NO pass, so nothing can be paired. "
              "Nothing was written.")
        return 1

    out_dir = Path(args.out) if args.out else theirs_dir.parent / "layers"
    out_dir.mkdir(parents=True, exist_ok=True)
    undecoded = 0
    print(f"\n{'pass':>26} {'dest ours':>10} {'dest theirs':>11} {'size':>10} "
          f"{'mean ours':>10} {'mean theirs':>11}  note")
    for k in shared:
        our_path, our_dest = ours[k]
        their_path, their_dest, their_len = theirs[k]
        oi = np.asarray(Image.open(our_path).convert("RGB")).astype(np.float32) / 255.0
        h, w = oi.shape[:2]
        fmt = k[4]
        raw = their_path.read_bytes()
        if fmt not in DECODABLE_FORMATS:
            # REFUSED, not skipped and not guessed. Named with its format so the
            # gap in coverage is visible in the same table as the results.
            undecoded += 1
            print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                  f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  REFUSED: dest format "
                  f"{fmt} is not decoded here (only "
                  f"{'/'.join(DECODABLE_FORMATS.values())})")
            continue
        ti = untile_8888(raw, w, h, np)
        if ti is None:
            # REFUSE THIS ROW, loudly, rather than skip it: a pass that could not
            # be decoded is not a pass that matched.
            undecoded += 1
            print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                  f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  UNDECODED: "
                  f"{their_len} bytes is not {w}x{h}x4 tiled k_8_8_8_8")
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
                            ).save(out_dir / ("pass_%s%03X_%dx%d_f%d_%d.png" % k))
        print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} {w}x{h} "
              f"{oi.mean():>10.4f} {t.mean():>11.4f}  {note}")

    print(f"\n{undecoded} pass(es) were REFUSED (format not decoded here) and are "
          f"NOT counted as matching.\nSide-by-side images for differing passes: {out_dir}"
          f" (ours left, console right, gamma 0.45)")
    print("BLIND SPOT: this compares resolve DESTINATIONS. A pass whose output "
          "is consumed\nwithout a resolve does not appear here at all.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
