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
          format. Current names include `_b<TEXTURE_BASE>_<RANGE_ADDR>_<LEN>`
          so an arbitrary partial tiled range can be located without guessing.
  ours    GEARS_DRAW_RESOLVE_DUMP_EACH=1. Our renderer resolves into HOST images
          keyed by destination address; it never writes guest memory, so there
          are no guest bytes on our side to compare byte-for-byte. Ours arrive
          already untiled, as resolve_<ord>_src<C|D><base>_<w>x<h>_<ADDR>_
          draw<n>.ppm.

THE JOIN KEY IS THE PASS'S STRUCTURAL IDENTITY: which EDRAM surface the copy
reads (RB_COPY_CONTROL.copy_src_select -> RB_COLOR[n]_INFO or RB_DEPTH_INFO),
the destination's guest dimensions (RB_COPY_DEST_PITCH) and its FORMAT, plus the
ordinal among the copies that share that key. Every one of those comes from the
guest's own registers, so the two emulators necessarily agree on them.

A DEPTH COPY'S FORMAT IS NOT RB_COPY_DEST_INFO.copy_dest_format. The hardware
takes it from RB_DEPTH_INFO.depth_format, and Xenia does the same (draw_util.cc
GetResolveInfo overwrites copy_dest_format with
DepthRenderTargetToTextureFormat(depth_format) when the source is depth), so the
console names this title's depth passes f23 (k_24_8_FLOAT) and f22 (k_24_8)
while the raw register reads 6 on both sides. Our renderer applies the same rule.
Getting this wrong does not misreport a value -- it makes the depth passes fail
to pair at all, and a pass with no counterpart reads as one the other side never
executed (catalog #90).

Depth f22/f23 passes are decoded and coarsely compared; every other depth format
is paired but explicitly left value-uncompared rather than guessed.

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

from layer_compare_ranges import (
    infer_legacy_texture_bases,
    merge_bands,
    selftest_legacy_texture_bases,
)
from xenos_tiled import stored_rows, tiled_offset_2d, untile, untile_range

OURS_RE = re.compile(
    r"resolve_(\d+)_src([CD])([0-9A-F]{3})_(\d+)x(\d+)_f(\d+)_([0-9a-f]{8})"
    r"_draw(\d+)\.ppm$", re.I)
# The trailing _e<N> is RB_COPY_DEST_INFO.copy_dest_endian and is OPTIONAL only
# so that captures taken before the oracle recorded it still parse -- they are
# then REFUSED per pass rather than decoded under an assumption, because
# assuming it is exactly what produced a retracted finding (catalog #96).
THEIRS_RE = re.compile(
    r"oracle_f(\d+)_copy(\d+)_src([CD])([0-9A-F]{3})_(\d+)x(\d+)_f(\d+)"
    r"(?:_e(\d+))?(?:_b([0-9A-F]{8}))?_([0-9A-F]{8})_(\d+)\.bin$", re.I)

# RB_COPY_DEST_INFO.copy_dest_format values this tool can DECODE, as
# (name, bytes per pixel). Anything not here is REFUSED per pass rather than
# read as 8888, because an eight-byte buffer read as four-byte bytes produces a
# recognisable image of the wrong thing. That mistake was made here first:
# three rows of the first paired run reported a difference for buffers this
# tool had mis-decoded, and the refusal is why it stopped happening.
#
# It began with k_8_8_8_8 alone, which left 9 of a gameplay frame's 16 shared
# passes unjudged -- the whole HDR half of the chain, since UE3 resolves its
# scene colour to k_16_16_16_16_FLOAT. The values are xenos.h's TextureFormat.
DECODABLE_FORMATS = {
    6: ("k_8_8_8_8", 4),
    7: ("k_2_10_10_10", 4),
    25: ("k_16_16", 4),
    32: ("k_16_16_16_16_FLOAT", 8),
}

def depth24_to_float(d24, is_float24, np):
    """The guest's 24-bit depth field -> float, both encodings.

    unorm24 (k_24_8) is d/16777215. float24 (k_24_8_FLOAT) is Xenos 20e4, and
    this is a VECTORISED PORT of Depth20e4To32 in runtime/gpu_draw_pixels.cpp --
    the same function the depth resolve uses -- rather than an approximation,
    because a wrong exponent bias makes a depth buffer that LOOKS like a depth
    buffer. Checked below against the three values that pin it: 0 -> 0,
    0xFFFFFF -> the largest representable, and a mid exponent.
    """
    if not is_float24:
        return d24.astype(np.float32) / 16777215.0
    f24 = d24.astype(np.uint32)
    exponent = (f24 >> 20) & 0xF
    mantissa = f24 & 0xFFFFF
    # The denormal branch: msb = 31 - clz(mantissa), which numpy expresses as
    # the floor of log2 for the non-zero lanes.
    safe = np.maximum(mantissa, 1)
    msb = (np.floor(np.log2(safe.astype(np.float64)))).astype(np.int64)
    denorm_exp = (msb - 19).astype(np.int64)
    denorm_man = (mantissa.astype(np.uint32) << (20 - msb).astype(np.uint32)) & 0xFFFFF
    unbiased = np.where(exponent != 0, exponent.astype(np.int64),
                        np.where(mantissa != 0, denorm_exp, -112))
    f32man = np.where(exponent != 0, mantissa,
                      np.where(mantissa != 0, denorm_man, 0)).astype(np.uint32)
    biased = ((unbiased + 112) & 0xFF).astype(np.uint32)
    bits = ((f32man & 0xFFFFF) | (biased << 20)) << 3
    return bits.astype(np.uint32).view(np.float32) if bits.dtype == np.uint32 \
        else bits.astype(np.uint32).view(np.float32)


def unpack_dest(px, fmt, np, endian=0):
    """Decoded bytes -> HxWx3 float32, in the same space as our PPM (0..1+).

    px is HxWxN uint8 straight out of the untiler, N being the format's bytes
    per pixel. Each branch is the guest's own packing, and a format with no
    branch cannot reach here -- DECODABLE_FORMATS gates it.
    """
    # THE ENDIAN APPLIES TO THE FOUR-BYTE FORMATS TOO. It was applied to the
    # eight-byte one and to depth and NOWHERE ELSE, so every k_2_10_10_10 dump
    # in this title -- all of them k8in32 -- was decoded with its dword's bytes
    # in the wrong order, which scrambles the bit fields: the 2-bit alpha lands
    # in the low bits and comes out as RED, 96.9% of it zero. That is exactly
    # what "two k_2_10_10_10 copies of the scene are near-black on our side"
    # was measuring (catalog #95) -- on the CONSOLE's side of the comparison.
    # The same mistake, in the same file, one format wider, was retracted as
    # catalog #96. b = the dword's bytes in guest order.
    b0, b1, b2, b3 = (px[..., i].astype(np.uint32) for i in range(4))
    if endian == 2:                                # k8in32: the dword reverses
        b0, b1, b2, b3 = b3, b2, b1, b0
    elif endian == 1:                              # k8in16: within each half
        b0, b1, b2, b3 = b1, b0, b3, b2
    w32 = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
    if fmt == 6:                                   # k_8_8_8_8, 8-bit unorm x4
        return np.stack([(w32 & 0xFF).astype(np.float32) / 255.0,
                         ((w32 >> 8) & 0xFF).astype(np.float32) / 255.0,
                         ((w32 >> 16) & 0xFF).astype(np.float32) / 255.0],
                        axis=-1)
    if fmt == 7:                                   # k_2_10_10_10
        r = (w32 & 0x3FF).astype(np.float32) / 1023.0
        g = ((w32 >> 10) & 0x3FF).astype(np.float32) / 1023.0
        b = ((w32 >> 20) & 0x3FF).astype(np.float32) / 1023.0
        return np.stack([r, g, b], axis=-1)
    if fmt == 25:                                  # k_16_16, two channels only
        r = (w32 & 0xFFFF).astype(np.float32) / 65535.0
        g = ((w32 >> 16) & 0xFFFF).astype(np.float32) / 65535.0
        return np.stack([r, g, np.zeros_like(r)], axis=-1)
    if fmt == 32:                                  # k_16_16_16_16_FLOAT
        h = px.view(np.uint8).reshape(px.shape[0], px.shape[1], 8)
        # k8in16 (endian 1) swaps the bytes WITHIN each 16-bit half, which is
        # what every 8-byte destination in this title uses. Reading one of them
        # the other way gives a range of -34368..34400 and a mean 15x the truth
        # (catalog #96), so the two orders are not close enough to guess between.
        if endian == 1:
            halves = ((h[..., 0::2].astype(np.uint16) << 8)
                      | h[..., 1::2].astype(np.uint16))
        else:
            halves = (h[..., 0::2].astype(np.uint16)
                      | (h[..., 1::2].astype(np.uint16) << 8))
        return halves[..., :3].view(np.float16).astype(np.float32)
    raise AssertionError(f"format {fmt} is in DECODABLE_FORMATS with no unpack")


def atlas_tiles(ours_dir, source_base):
    """The scissor rectangles the frame's own draws rendered into a depth base.

    Read from the per-draw table (GEARS_DRAW_DIAG) written by the SAME capture
    run, which is why layer_capture.sh takes one. A depth atlas holds several
    lights' shadow maps side by side, and a mean over the whole 864x864 hides
    which of them is wrong: measured, seven tiles agreed (one to 0.0003) while
    the eighth was UNWRITTEN at exactly the cleared value, and the whole-atlas
    number for that pass was a bland 0.246 (catalog #91).

    Returns [] when there is no table, which is reported rather than passed off
    as "no tiles" -- an atlas with no breakdown and an atlas with one tile are
    not the same statement.
    """
    tsv = Path(ours_dir) / "draws.tsv"
    if not tsv.is_file():
        return None
    rects, cols = {}, None
    for line in tsv.read_text(errors="replace").splitlines():
        f = line.split("\t")
        if cols is None:
            cols = {n: i for i, n in enumerate(f)}
            if "depth_base" not in cols or "sc_w" not in cols:
                return None            # a table from before those columns
            continue
        try:
            if int(f[cols["depth_base"]], 16) != source_base:
                continue
            r = (int(f[cols["sc_x"]]), int(f[cols["sc_y"]]),
                 int(f[cols["sc_w"]]), int(f[cols["sc_h"]]))
        except (ValueError, IndexError):
            continue
        rects[r] = rects.get(r, 0) + 1
    return sorted(rects.items())


def key_str(k):
    src, base, w, h, fmt, nth = k
    return f"src{src}{base:03X} {w}x{h} f{fmt} #{nth}"


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
    # EMPTIED FIRST. A self-test that runs in a directory holding a previous
    # run's files is testing the union of the two: when the dump names gained
    # an endian field, the old and new names for the SAME copy both matched,
    # the ordinal paired ours #1 against theirs' second copy of #0, and the
    # difference case reported a match. The test caught it -- and only because
    # it has a difference case at all.
    for d in (ours_dir, theirs_dir):
        d.mkdir(parents=True, exist_ok=True)
        for old_file in d.iterdir():
            if old_file.is_file():
                old_file.unlink()
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
         f"oracle_f1_copy{nth}_srcC000_{w}x{h}_f6_e0_00000000_{w * h * 4}.bin"
         ).write_bytes(bytes(raw))
        Image.fromarray(img[..., :3]).save(
            ours_dir /
            f"resolve_{nth:02}_srcC000_{w}x{h}_f6_00000000_draw{nth}.ppm")
        back = untile(bytes(raw), w, h, np)
        if back is None:
            print(f"SELFTEST FAIL: the {note} case did not decode at all")
            ok = False
            continue
        same = bool((back[..., :3] == img[..., :3]).all())
        want = (note == "identical")
        print(f"selftest: {note} case decodes {'equal' if same else 'different'}"
              f" (expected {'equal' if want else 'different'})")
        ok = ok and same == want
    # THE WIDE FORMAT, tiled at ITS bytes per pixel. This is the case the file
    # header warns about: an eight-byte destination tiles on a different stride,
    # and an "almost right" swizzle produces a recognisable image of the wrong
    # thing. Round-tripped through the SCALAR tiler at log2_bpp 3, so the
    # vectorised untiler is checked against the ported address rather than
    # against itself.
    fw, fh = 32, 32
    half = np.stack([
        (np.arange(fh)[:, None] + np.zeros(fw)) / 32.0,
        (np.zeros(fh)[:, None] + np.arange(fw)) / 32.0,
        np.full((fh, fw), 2.5),          # ABOVE 1.0: an HDR value must survive
        np.ones((fh, fw)),
    ], axis=-1).astype(np.float16)
    raw8 = bytearray(fw * fh * 8)
    hb = half.view(np.uint8).reshape(fh, fw, 8)
    for y in range(fh):
        for x in range(fw):
            off = tiled_offset_2d(x, y, fw, 3)
            raw8[off:off + 8] = bytes(hb[y, x])
    back8 = untile(bytes(raw8), fw, fh, np, 8)
    if back8 is None:
        print("SELFTEST FAIL: the wide-format case did not decode at all")
        ok = False
    else:
        got = unpack_dest(back8, 32, np)
        want = half[..., :3].astype(np.float32)
        good = bool(np.allclose(got, want))
        print(f"selftest: k_16_16_16_16_FLOAT round-trips through the 8-byte"
              f" tiler: {good} (expected True)")
        # ... and the HDR value is not clamped on the way through.
        hdr = float(got[..., 2].max())
        print(f"selftest: its above-1.0 channel survives decoding: {hdr:.2f}"
              f" (expected 2.50)")
        ok = ok and good and abs(hdr - 2.5) < 1e-3
    # THE 32-BIT UNPACKS, each against an arithmetic answer. A format in
    # DECODABLE_FORMATS with a wrong unpack produces a plausible image, which is
    # the failure this whole file is written against.
    packed = np.zeros((1, 1, 4), dtype=np.uint8)
    packed[0, 0] = [0xFF, 0x03, 0x00, 0x00]     # 0x000003FF
    got7 = unpack_dest(packed, 7, np)[0, 0]
    ok7 = abs(got7[0] - 1.0) < 1e-6 and abs(got7[1]) < 1e-6
    print(f"selftest: k_2_10_10_10 puts 0x3FF in RED and 0 in green:"
          f" {tuple(round(float(v), 3) for v in got7)} -> {ok7} (expected True)")
    packed[0, 0] = [0x00, 0x80, 0xFF, 0xFF]     # r = 0x8000, g = 0xFFFF
    got25 = unpack_dest(packed, 25, np)[0, 0]
    ok25 = abs(got25[0] - 0.5000076) < 1e-4 and abs(got25[1] - 1.0) < 1e-6
    print(f"selftest: k_16_16 puts 0x8000 in RED and 0xFFFF in green:"
          f" {tuple(round(float(v), 4) for v in got25)} -> {ok25} (expected True)")
    ok = ok and ok7 and ok25

    # THIRD CASE: a DEPTH pass must be joined and must be reported as
    # not-value-compared. Both halves matter -- a depth pair silently dropped
    # from the join reads as "only the console resolves it", which is the exact
    # false reading (issue #90) that the depth snapshots were added to end.
    dw, dh = 32, 32
    (theirs_dir / f"oracle_f1_copy9_srcD000_{dw}x{dh}_f6_e0_00000000_{dw * dh * 4}.bin"
     ).write_bytes(bytes(dw * dh * 4))
    Image.fromarray(np.zeros((dh, dw, 3), dtype=np.uint8)).save(
        ours_dir / f"resolve_09_srcD000_{dw}x{dh}_f6_00000000_draw9.ppm")
    # A DEPTH PASS THAT IS NOW COMPARED, which the format-6 one above is not:
    # it takes the "not a depth format this decodes" path and so leaves the
    # decode-and-compare branch untested. k_24_8 (22), endian 8in32, a ramp
    # whose float value is known by construction, and OUR side written as the
    # same ramp in grey -- so it must MATCH. A tool that decoded depth wrongly
    # would differ here, and one that skipped depth entirely would not print a
    # comparison at all.
    kw, kh = 32, 32
    ramp = (np.arange(kh)[:, None] * np.ones(kw)) / float(kh)   # 0 .. 1
    d24 = np.clip((ramp * 16777215.0).round(), 0, 16777215).astype(np.uint32)
    word = (d24 << 8)                       # depth in the high 24 bits
    raw24 = bytearray(kw * kh * 4)
    for y in range(kh):
        for x in range(kw):
            v = int(word[y, x])
            # 8in32: the dword's bytes come out reversed.
            b = bytes([(v >> 24) & 0xFF, (v >> 16) & 0xFF,
                       (v >> 8) & 0xFF, v & 0xFF])
            off = tiled_offset_2d(x, y, kw, 2)
            raw24[off:off + 4] = b
    (theirs_dir / f"oracle_f1_copy8_srcD001_{kw}x{kh}_f22_e2_00000000_"
                  f"{kw * kh * 4}.bin").write_bytes(bytes(raw24))
    Image.fromarray((np.repeat(ramp[..., None], 3, axis=-1) * 255)
                    .astype(np.uint8)).save(
        ours_dir / f"resolve_08_srcD001_{kw}x{kh}_f22_00000000_draw8.ppm")

    # A legacy capture may omit `_b<TEXTURE_BASE>` on every file. A complete
    # sibling still pins the base for a partial range of the same structural
    # destination. Test the inference directly, including its refusal to infer
    # when two complete siblings disagree about the base.
    legacy_ok, ambiguous_ok = selftest_legacy_texture_bases()
    print(f"selftest: legacy partial ranges inherit one proven complete-sibling"
          f" texture base: {legacy_ok} (expected True)")
    print(f"selftest: ambiguous legacy bases are refused: {ambiguous_ok}"
          f" (expected True)")
    ok = ok and legacy_ok and ambiguous_ok

    # The oracle probes the exact contiguous memory range Xenia says a resolve
    # may modify. A rectangle in a tiled texture is not necessarily a whole
    # number of linear rows. Base + range address metadata must locate those
    # bytes and compare only the complete texels actually present.
    partial_start, partial_end = 512, 3073
    partial = raw24[partial_start:partial_end]
    (theirs_dir / f"oracle_f1_copy31_srcD002_{kw}x{kh}_f22_e2_b00001000_"
                  f"{0x1000 + partial_start:08X}_{len(partial)}.bin").write_bytes(partial)
    Image.fromarray((np.repeat(ramp[..., None], 3, axis=-1) * 255)
                    .astype(np.uint8)).save(
        ours_dir / f"resolve_31_srcD002_{kw}x{kh}_f22_00001000_draw31.ppm")

    # FOURTH CASE: a SECOND console frame that agrees BETTER and is structurally
    # WRONG. The window the oracle now dumps exists because equal frame indices
    # are not equal game time, and the frame is chosen on pass STRUCTURE -- so
    # the way this choice can fail silently is by drifting to "whichever frame
    # compares best", which would make every future comparison self-confirming.
    # Frame 2 here holds ONE pass, byte-identical to ours, where frame 1 holds
    # four including the shifted one that must be reported as differing. A tool
    # that picked by agreement would choose frame 2, print a clean table, and
    # lose the difference entirely.
    raw_same = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            off = tiled_offset_2d(x, y, w, 2)
            raw_same[off:off + 4] = bytes(img[y, x])
    (theirs_dir /
     f"oracle_f2_copy0_srcC000_{w}x{h}_f6_e0_00000000_{w * h * 4}.bin"
     ).write_bytes(bytes(raw_same))

    # FIFTH CASE: a console pass resolved in two BANDS must be rejoined, and a
    # buffer that is NOT contiguous with it must NOT be. The join is arithmetic
    # -- same source, width and format, destination exactly one band of bytes
    # on -- and the failure it guards against is joining two unrelated buffers
    # into a plausible image, so both classes are run.
    bw, bh, b1 = 32, 48, 32          # 32 rows, then 16, at a 32-row alignment
    band_img = np.stack([(xs[:bh, :bw] * 5) % 256, (ys[:bh, :bw] * 5) % 256,
                         (xs[:bh, :bw] * ys[:bh, :bw]) % 256,
                         np.full((bh, bw), 255)], axis=-1).astype(np.uint8)
    for part, (y0, rows, dest) in enumerate(
            [(0, b1, 0x1000000), (b1, 32, 0x1000000 + b1 * bw * 4)]):
        rawb = bytearray(bw * rows * 4)
        for y in range(rows):
            for x in range(bw):
                src = band_img[y0 + y, x] if y0 + y < bh else band_img[-1, x]
                off = tiled_offset_2d(x, y, bw, 2)
                rawb[off:off + 4] = bytes(src)
        (theirs_dir / f"oracle_f1_copy{20 + part}_srcC111_{bw}x"
                      f"{bh if part == 0 else 16}_f6_e0_{dest:08X}_"
                      f"{bw * rows * 4}.bin").write_bytes(bytes(rawb))
    Image.fromarray(band_img[..., :3]).save(
        ours_dir / f"resolve_20_srcC111_{bw}x{bh}_f6_00000000_draw20.ppm")
    # ...and a buffer of the same shape at an address that is NOT contiguous.
    (theirs_dir / f"oracle_f1_copy22_srcC222_{bw}x16_f6_e0_09999999_"
                  f"{bw * 32 * 4}.bin").write_bytes(bytes(bw * 32 * 4))
    Image.fromarray(np.zeros((16, bw, 3), dtype=np.uint8)).save(
        ours_dir / f"resolve_22_srcC222_{bw}x16_f6_00000000_draw22.ppm")

    # SIXTH CASE: THE ENDIAN, on a four-byte format. It was applied to the
    # eight-byte format and to depth and to nothing else, so every
    # k_2_10_10_10 dump in this title -- all k8in32 -- had its dword's bytes in
    # the wrong order, which scrambles the bit fields rather than shifting a
    # value: the 2-bit alpha lands in the low bits and comes out as RED. That
    # read as "two copies of the scene are near-black on our side" for a whole
    # session (catalog #95), and the identical mistake one format wider had
    # already been retracted once (catalog #96). Both classes here: the SAME
    # pixel written in guest order under e0 and byte-reversed under e2 must
    # decode to the SAME colour, and each must be wrong under the other tag.
    px = np.array([[0x11, 0x22, 0x33, 0x44]], dtype=np.uint8).reshape(1, 1, 4)
    rev = px[..., ::-1].copy()
    plain = unpack_dest(px, 7, np, 0)[0, 0]
    swapped = unpack_dest(rev, 7, np, 2)[0, 0]
    same = bool(np.allclose(plain, swapped))
    print(f"selftest: k_2_10_10_10 under k8in32 decodes the reversed bytes to"
          f" the same colour: {same} (expected True)")
    # ...and the tag is not decorative: the wrong one must give a DIFFERENT
    # answer, or the test above would pass on a decoder that ignored it.
    differs = not bool(np.allclose(unpack_dest(rev, 7, np, 0)[0, 0], plain))
    print(f"selftest: ...and reading those same bytes as k8in16 does NOT:"
          f" {differs} (expected True)")
    ok = ok and same and differs

    # Pitch padding must be untiled for row addressing and then ignored.
    pitch_w, logical_w, logical_h = 64, 48, 32
    logical = np.stack([
        (np.arange(logical_w)[None, :] * 5 + np.zeros((logical_h, 1))) % 256,
        (np.arange(logical_h)[:, None] * 7 + np.zeros((1, logical_w))) % 256,
        np.full((logical_h, logical_w), 91),
        np.full((logical_h, logical_w), 255),
    ], axis=-1).astype(np.uint8)
    stored = np.full((logical_h, pitch_w, 4), 237, dtype=np.uint8)
    stored[:, :logical_w] = logical
    pitched_raw = bytearray(pitch_w * logical_h * 4)
    for y in range(logical_h):
        for x in range(pitch_w):
            off = tiled_offset_2d(x, y, pitch_w, 2)
            pitched_raw[off:off + 4] = bytes(stored[y, x])
    oracle_name = (f"oracle_f1_copy30_srcC333_{pitch_w}x{logical_h}_f6_e0_"
                   f"03000000_{len(pitched_raw)}.bin")
    (theirs_dir / oracle_name).write_bytes(pitched_raw)
    native_name = f"resolve_30_srcC333_{pitch_w}x{logical_h}_f6_03000000_draw30.ppm"
    Image.fromarray(logical[..., :3]).save(ours_dir / native_name)

    # Run the REAL comparison over these three pairs and read what it printed --
    # checking the filename patterns alone would leave the depth branch itself
    # unexercised, which is the branch being proven.
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        main(["layer_compare", "--ours", str(ours_dir), "--theirs",
              str(theirs_dir), "--out", str(work / "st_layers")])
    out = buf.getvalue()
    checks = [
        ("the depth pass is JOINED, not listed as one-sided",
         "srcD000" not in out.split("passes both sides resolve:")[1]
         .split("\n\n")[0]),
        ("the depth pass is reported as not value-compared",
         "values NOT compared" in out),
        ("all depth passes are counted, and the summary separates those it"
         " compared from the one it could not",
         "3 DEPTH pass(es) paired on both sides, 2 of them value-compared"
         " and 1 not" in out),
        ("a k_24_8 DEPTH pass is decoded and COMPARED, and matches",
         any("srcD001" in ln and "match" in ln
             for ln in out.splitlines())),
        ("an arbitrary partial tiled range is located from texture base"
         " metadata and compared only over available texels",
         any("srcD002" in ln and "match" in ln and
             "% of tiled texels" in ln for ln in out.splitlines())),
        ("the colour difference still fires alongside it",
         "DIFFER" in out),
        ("the console frame is chosen on STRUCTURE, not on agreement: the"
         " 4-pass frame wins over a 1-pass frame that matches perfectly",
         any("frame 1:" in ln and "<- used" in ln for ln in out.splitlines())),
        ("a pass the console resolved in two BANDS is rejoined and compared"
         " over its WHOLE height, not just the first band",
         any("srcC111" in ln and "32x48" in ln and "match" in ln
             and "rows are in the console's buffer" not in ln
             for ln in out.splitlines())
         and any("srcC111" in ln and "in bands" in ln
                 for ln in out.splitlines())),
        ("...and a buffer that is NOT contiguous with it is left alone",
         any("srcC222" in ln and "32x16" in ln for ln in out.splitlines())
         and not any("srcC222" in ln and "in bands" in ln
                     for ln in out.splitlines())),
        ("...and choosing it KEPT the difference that the flattering frame"
         " would have hidden",
         "srcC000" in out and any("srcC000" in ln and "DIFFER" in ln
                                  for ln in out.splitlines())),
        ("guest pitch padding is untiled but excluded from the sampled logical"
         " image comparison",
         any("srcC333" in ln and "48x32" in ln and "match" in ln
             for ln in out.splitlines())
         and not any("srcC333" in ln and ("UNDECODED" in ln or "DIFFER" in ln)
                     for ln in out.splitlines())),
    ]
    for note, passed in checks:
        print(f"selftest: {note}: {passed} (expected True)")
        ok = ok and passed
    print("SELFTEST PASS: the untiler round-trips, the difference test fires,"
          " and a depth pass joins"
          if ok else "SELFTEST FAIL: do not trust this tool's output")
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours")
    ap.add_argument("--theirs")
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
        work = Path(args.ours) if args.ours else Path("scratch/layer_compare_selftest")
        return selftest(work, np, Image)

    if not args.ours or not args.theirs:
        print("REFUSING: --ours and --theirs are required. Nothing was compared.")
        return 2

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
    # THE CONSOLE MAY HAVE DUMPED SEVERAL FRAMES, and they are kept apart.
    # Both emulators advance the guest by WALL-CLOCK delta time, so the same
    # frame INDEX is not the same game time: two runs of the oracle dumped
    # their frames 875 and 873 with a DIFFERENT NUMBER OF SHADOW-CASTING
    # LIGHTS, and every per-pass number in the second run was read as a
    # renderer difference when the two sides were not looking at the same
    # scene. With a window (GEARS_ORACLE_DUMP_FRAMES) the frame is CHOSEN and
    # the choice is shown.
    by_frame = {}
    for p in sorted(theirs_dir.iterdir(),
                    key=lambda q: int(THEIRS_RE.search(q.name).group(2))
                    if THEIRS_RE.search(q.name) else -1):
        m = THEIRS_RE.search(p.name)
        if not m:
            continue
        frame = int(m.group(1))
        k = (m.group(3).upper(), int(m.group(4), 16), int(m.group(5)),
             int(m.group(6)), int(m.group(7)))
        d = by_frame.setdefault(frame, ({}, {}))
        nth = d[1].get(k, 0)
        d[1][k] = nth + 1
        # Base metadata is optional only for older complete-range captures.
        endian = int(m.group(8)) if m.group(8) is not None else None
        base = int(m.group(9), 16) if m.group(9) is not None else None
        d[0][k + (nth,)] = (p, int(m.group(10), 16), int(m.group(11)), endian,
                            base, [])

    color_bpp = {fmt: spec[1] for fmt, spec in DECODABLE_FORMATS.items()}
    legacy_base_inferences = {}
    for frame, (dumps, _) in by_frame.items():
        legacy_base_inferences[frame] = infer_legacy_texture_bases(dumps, color_bpp)

    # THE CONSOLE RESOLVES A FULL-SCREEN SURFACE IN BANDS, because 1280x720 of
    # colour plus depth does not fit in 10 MiB of EDRAM. Rejoined here, per
    # frame, before anything is scored or paired -- otherwise the second band
    # reads as a pass only the console renders, and the first reads as a
    # 1280x512 buffer that quietly drops the bottom 29% of every full-screen
    # comparison in the frame.
    band_joins = []
    for frame, (dumps, _) in by_frame.items():
        for kept, joined in merge_bands(dumps, color_bpp):
            band_joins.append((frame, kept, joined))

    # CHOSEN BY STRUCTURE, NEVER BY AGREEMENT. The frame is picked on which
    # passes it contains -- the same set of copies means the same set of lights
    # and post steps -- and NOT on how small the differences come out, which
    # would be an instrument that selects the answer it wants to report.
    # Every candidate is printed with its score, so a choice that barely won
    # says so and a window where nothing aligned cannot be mistaken for a
    # window where the first frame was right.
    our_keys = set(ours)
    scored = []
    for frame, (dumps, _) in sorted(by_frame.items()):
        ks = set(dumps)
        scored.append((len(ks & our_keys), -len(ks ^ our_keys), frame, dumps))
    scored.sort(reverse=True)
    if len(by_frame) > 1:
        print(f"\nthe console dumped {len(by_frame)} frame(s); the one whose"
              f" PASS STRUCTURE is closest to ours is used, and the choice is"
              f" made on structure alone -- never on how well the values agree")
        for shared_n, neg_sym, frame, dumps in scored:
            print(f"  frame {frame}: {len(dumps)} pass(es), {shared_n} shared"
                  f" with ours, {-neg_sym} unmatched either way"
                  + ("   <- used" if frame == scored[0][2] else ""))
    theirs_frame = scored[0][2] if scored else None
    theirs = scored[0][3] if scored else {}
    # Never silent: a join changes what every row below is measuring.
    for frame, kept, joined in band_joins:
        if frame == theirs_frame:
            print(f"the console resolved {key_str(kept)} in bands;"
                  f" {key_str(joined)} is its continuation in guest memory and"
                  f" is joined onto it")
    for structural_key, base, count in legacy_base_inferences.get(theirs_frame, []):
        src, source_base, width, height, fmt = structural_key
        print(f"legacy oracle capture: inferred texture base {base:#x} for"
              f" {count} src{src}{source_base:03X} {width}x{height} f{fmt}"
              f" range(s) from the unique complete sibling dump")
    # Held, and printed next to the pass lists it qualifies rather than above
    # the dump counts, because it is a caveat on the TABLE.
    alignment_note = None
    if scored and -scored[0][1] != 0:
        # WHICH KIND OF UNMATCHED IS IT. A pass missing from EVERY frame of the
        # window is not a timing artefact -- the console rendered it in all of
        # them and we rendered it in none, which is a renderer difference and
        # the most interesting row this tool can produce. A pass missing from
        # SOME frames is the scene moving under the comparison. Saying only
        # "not aligned" for both would bury a real gap under a caveat: the
        # first window measured here had two 1280x208 passes the port never
        # renders, in all five frames, and the undifferentiated note called
        # that a possible difference of scene.
        unmatched_each = [set(d) ^ our_keys for _, _, _, d in scored]
        always = set.intersection(*unmatched_each) if unmatched_each else set()
        varies = set.union(*unmatched_each) - always if unmatched_each else set()
        parts = [f"NOTE: no dumped console frame has our pass structure -- the"
                 f" best is frame {theirs_frame} with {-scored[0][1]} pass(es)"
                 f" unmatched."]
        if always:
            parts.append(
                f" UNMATCHED IN EVERY ONE of the {len(scored)} dumped frame(s),"
                f" so this is a renderer difference and not the scene moving: "
                + ", ".join(key_str(k) for k in sorted(always)) + ".")
        if varies:
            parts.append(
                f" Unmatched in SOME frames only, which is the scene moving"
                f" under the comparison: "
                + ", ".join(key_str(k) for k in sorted(varies))
                + ". A per-pass difference below may be a difference of SCENE"
                  " rather than of renderer.")
        alignment_note = "".join(parts)

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
    if alignment_note:
        print("\n" + alignment_note)
    if not shared:
        print("REFUSING: the two sides share NO pass, so nothing can be paired. "
              "Nothing was written.")
        return 1

    out_dir = Path(args.out) if args.out else theirs_dir.parent / "layers"
    out_dir.mkdir(parents=True, exist_ok=True)
    undecoded = 0
    depth_pairs = 0
    depth_compared = 0
    print(f"\n{'pass':>26} {'dest ours':>10} {'dest theirs':>11} {'size':>10} "
          f"{'mean ours':>10} {'mean theirs':>11}  note")
    for k in shared:
        our_path, our_dest = ours[k]
        (their_path, their_dest, their_len, their_endian, their_base,
         their_extra) = theirs[k]
        oi = np.asarray(Image.open(our_path).convert("RGB")).astype(np.float32) / 255.0
        h, w = oi.shape[:2]
        # Untile at the key's guest pitch, then crop to the native image's
        # logical sampled width (322 pixels at pitch 352 for UE3 bloom).
        guest_w = k[2]
        fmt = k[4]
        raw = their_path.read_bytes()

        # EACH BAND IS TILED IN ITS OWN DESTINATION, so they are untiled
        # separately and stacked -- concatenating the raw bytes and untiling
        # once would swizzle across the seam and produce a plausible image of
        # the wrong thing.
        def decode_range(data, address, base, height, bpp):
            rows = stored_rows(len(data), guest_w, bpp)
            if base is not None and (address != base or rows is None):
                try:
                    pixels, valid = untile_range(
                        data, guest_w, height, address - base, np, bpp)
                except ValueError:
                    return None, 0, None
                return pixels, height, valid
            pixels = untile(data, guest_w, rows, np, bpp) if rows else None
            valid = np.ones((rows, guest_w), dtype=bool) if pixels is not None else None
            return pixels, rows or 0, valid

        def their_image(bpp, _raw=raw, _extra=their_extra):
            first, rows, first_valid = decode_range(
                _raw, their_dest, their_base, h, bpp)
            if first is None:
                return None, 0, None
            bands, masks, total = [first], [first_valid], rows
            for p2, d2, _l2, _e2, b2 in _extra:
                b = p2.read_bytes()
                t2, r2, v2 = decode_range(b, d2, b2, h - total, bpp)
                if t2 is None:
                    return None, 0, None
                bands.append(t2)
                masks.append(v2)
                total += r2
            pixels = (bands[0] if len(bands) == 1
                      else np.concatenate(bands, axis=0))
            valid = (masks[0] if len(masks) == 1
                     else np.concatenate(masks, axis=0))
            return pixels, total, valid
        # A DEPTH destination is present on both sides but NOT comparable here,
        # and saying "DIFFER" for one would be the tool's own defect reported as
        # the renderer's. The two sides hold different things: ours is the host
        # R32 depth written out as grey, theirs is the guest's packed depth
        # bytes, whose layout across the destination's components is exactly the
        # open question in catalog #35. Decoding it as k_8_8_8_8 (which its
        # copy_dest_format nominally is) produces a plausible image of the wrong
        # quantity. The row is printed so the pass's PRESENCE on both sides --
        # the thing this join now establishes -- is visible.
        if k[0] == "D":
            depth_pairs += 1
            # DEPTH IS COMPARABLE NOW, for the two formats the guest uses. It
            # was not while the tool could not decode the console's bytes: ours
            # is the host depth written out as grey and theirs is the guest's
            # packed 24:8 dword, and the layout across those bytes was the open
            # part of catalog #35. It is k_24_8 (22, unorm24) or k_24_8_FLOAT
            # (23, Xenos 20e4) with the depth in the HIGH 24 bits, 4 bytes per
            # pixel, endian 8in32 -- the same layout the aliasing shader packs.
            # A format outside those two is still refused rather than guessed.
            if fmt not in (22, 23) or their_endian is None:
                print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                      f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  BOTH SIDES "
                      f"RESOLVE THIS, values NOT compared: "
                      + (f"dest format {fmt} is not a depth format this decodes"
                         if fmt not in (22, 23)
                         else "this capture does not record copy_dest_endian"))
                continue
            ti, rows, available = their_image(4)
            if ti is None:
                print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                      f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  UNDECODED: "
                      f"{their_len} bytes does not describe whole rows and the"
                      f" capture has no texture-base metadata for locating its"
                      f" partial tiled range")
                continue
            dshort = ""
            if rows > h:
                ti = ti[:h]           # padding to the tile alignment
                available = available[:h]
            elif rows < h:
                # SHORT, exactly as a colour destination can be: the shadow-map
                # copies hold 672 of their 864 rows. Compare the rows that
                # exist and say how many -- a partial pass must never read as a
                # whole one.
                dshort = (f" [only {rows} of {h} rows are in the console's"
                          f" buffer; compared over those]")
                oi = oi[:rows]
                h = rows
            if not available.any():
                print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                      f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  UNDECODED: the"
                      f" dumped byte range contains no complete texels")
                continue
            if not available.all():
                dshort += (f" [{100 * available.mean():.1f}% of tiled texels"
                           f" are present in this byte range]")
            # 8in32: the dword's bytes are reversed on the way out.
            word = (ti[..., 3].astype(np.uint32)
                    | (ti[..., 2].astype(np.uint32) << 8)
                    | (ti[..., 1].astype(np.uint32) << 16)
                    | (ti[..., 0].astype(np.uint32) << 24))
            if their_endian != 2:
                word = (ti[..., 0].astype(np.uint32)
                        | (ti[..., 1].astype(np.uint32) << 8)
                        | (ti[..., 2].astype(np.uint32) << 16)
                        | (ti[..., 3].astype(np.uint32) << 24))
            td = depth24_to_float(word >> 8, fmt == 23, np)
            # OUR SIDE IS 8-BIT, so this is a coarse comparison and says so: it
            # answers "is this the same depth buffer", not "is it exact".
            od = oi[..., 0]           # our depth target is written out as grey
            delta = np.abs(np.clip(td, 0.0, 1.0) - od)
            d = float(delta[available].mean())
            # A DESTINATION IS NOT ALL WRITTEN. A copy writes a RECTANGLE into a
            # texture -- the shadow atlas takes a 448x448 region of an 864-wide
            # one -- and the rest is whatever the guest's memory held, which is
            # zeros; ours is a host image whose unwritten area holds whatever it
            # holds. Comparing there says nothing about either renderer, so the
            # row carries BOTH numbers. When they disagree it is the second that
            # means something: the shadow maps report 0.196 over the whole
            # destination and 0.0084 over the part the console actually wrote.
            wrote = available & (td >= 0.01)
            note = ("match" if d < 0.02 else f"DIFFER, mean |d| {d:.3f}")
            if wrote.any() and not np.array_equal(wrote, available):
                dw = float(delta[wrote].mean())
                note += (f"; over the {100 * wrote.sum() / available.sum():.1f}%"
                         f" of available texels the console did"
                         f" NOT leave at zero, mean |d| {dw:.4f}"
                         + (" -- they agree where both wrote" if dw < 0.02
                            else ""))
            note += (" [depth: theirs decoded from the guest's 24:8;"
                     " ours is an 8-bit grey dump, so this is coarse]" + dshort)
            depth_compared += 1
            # PER TILE, when the capture carries its own draw table. A depth
            # atlas holds several lights' shadow maps, and one of them being
            # UNWRITTEN reads as a mild whole-atlas difference.
            tiles = atlas_tiles(ours_dir, k[1])
            tile_lines = []
            if tiles is None:
                tile_lines.append(
                    f"       (no per-draw table in {ours_dir}, so this pass is"
                    f" not broken down by tile -- re-capture with the current"
                    f" layer_capture.sh)")
            for (tx, ty, tw, th), n in tiles or []:
                if tw >= w and th >= h:
                    continue              # the full-surface rect, not a tile
                if ty + th > h or tx + tw > w:
                    tile_lines.append(
                        f"       tile x{tx} y{ty} {tw}x{th}: past the {w}x{h}"
                        f" the console's buffer holds, NOT compared")
                    continue
                ta = od[ty:ty + th, tx:tx + tw]
                tb = np.clip(td, 0.0, 1.0)[ty:ty + th, tx:tx + tw]
                tv = available[ty:ty + th, tx:tx + tw]
                if not tv.any():
                    tile_lines.append(
                        f"       tile x{tx} y{ty} {tw}x{th}: no texels in the"
                        f" dumped byte range, NOT compared")
                    continue
                tw_wrote = tv & (tb >= 0.01)
                td_ = (float(np.abs(tb - ta)[tw_wrote].mean())
                       if tw_wrote.any() else float("nan"))
                empty = " -- UNWRITTEN ON OURS at the cleared value" if (
                    float(ta.min()) > 0.999) else ""
                tile_lines.append(
                    f"       tile x{tx:>4} y{ty:>4} {tw}x{th} ({n} draw(s)):"
                    f" console wrote {100 * tw_wrote.sum() / tv.sum():5.1f}%,"
                    f" ours {ta[tv].mean():.4f} theirs {tb[tv].mean():.4f},"
                    f" |d| {td_:.4f}{empty}")
            if d >= 0.02:
                side = np.concatenate([np.repeat(od[..., None], 3, axis=-1),
                                       np.repeat(np.clip(td, 0, 1)[..., None],
                                                 3, axis=-1)], axis=1)
                Image.fromarray((np.clip(side, 0, 1) ** 0.45 * 255)
                                .astype(np.uint8)
                                ).save(out_dir / ("pass_%s%03X_%dx%d_f%d_%d.png" % k))
            print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} {w}x{h} "
                  f"{od[available].mean():>10.4f} {td[available].mean():>11.4f}  {note}")
            for line in tile_lines:
                print(line)
            continue
        if fmt not in DECODABLE_FORMATS:
            # REFUSED, not skipped and not guessed. Named with its format so the
            # gap in coverage is visible in the same table as the results.
            undecoded += 1
            # THE LENGTH IS DATA, so say what it implies rather than discarding
            # it with the refusal. A refused pass whose buffer is exactly
            # w*h*8 is a decode waiting to be written; one that is not is a
            # destination whose layout is not what its dimensions suggest, and
            # that distinction is the whole of the next step (catalog #95).
            implied = ""
            if w and h:
                per = their_len / float(w * h)
                implied = (f"; its {their_len} bytes over {w}x{h} is"
                           f" {per:.3f} B/px"
                           + (" (a whole number, so the layout is plain)"
                              if abs(per - round(per)) < 1e-6 else
                              " (NOT a whole number, so this destination is not"
                              " w*h*bpp -- padded, tiled to a larger extent, or"
                              " a different height)"))
            print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                  f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  REFUSED: dest format "
                  f"{fmt} is not decoded here (only "
                  f"{'/'.join(n for n, _ in DECODABLE_FORMATS.values())})"
                  f"{implied}")
            continue
        fmt_name, bpp = DECODABLE_FORMATS[fmt]
        # NO ENDIAN, NO DECODE for a format whose byte order changes what the
        # values MEAN. An old capture (taken before the oracle recorded
        # copy_dest_endian) refuses here rather than being read under an
        # assumption -- which is the mistake this check exists because of.
        if bpp > 4 and their_endian is None:
            undecoded += 1
            print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                  f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  REFUSED: this capture"
                  f" does not record copy_dest_endian, and {fmt_name} cannot be"
                  f" read without it. Re-capture with the current oracle")
            continue
        # DECODE AT THE HEIGHT THE BUFFER ACTUALLY HAS, then crop or refuse.
        ti, rows, available = their_image(bpp)
        short = ""
        if ti is None:
            undecoded += 1
            print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                  f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  UNDECODED: "
                  f"{their_len} bytes does not describe whole rows and the"
                  f" capture has no texture-base metadata for locating its"
                  f" partial tiled range")
            continue
        if rows != h:
            if rows > h:
                ti = ti[:h]           # padding to the tile alignment
                available = available[:h]
            else:
                # SHORT: the guest's buffer holds fewer rows than the copy's
                # rectangle. Compare the rows that exist and say how many, so a
                # partial pass is never read as a whole one.
                short = (f" [only {rows} of {h} rows are in the console's"
                         f" buffer; compared over those]")
                oi = oi[:rows]
                h = rows
        if not available.any():
            undecoded += 1
            print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                  f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  UNDECODED: "
                  f"the dumped byte range contains no complete texels")
            continue
        # Pitch padding is not sampled content and may hold old EDRAM bits.
        ti = ti[:, :w]
        available = available[:, :w]
        if not available.all():
            short += (f" [{100 * available.mean():.1f}% of tiled texels are"
                      f" present in this byte range]")
        t = unpack_dest(ti, fmt, np, their_endian or 0)
        # OUR SIDE IS AN 8-BIT PPM, so it is already clamped to 0..1. A float
        # destination on the console is not, and this frame's scene colour
        # reaches 3.66 -- comparing them raw reports the PPM's clamp as the
        # renderer's difference. Both sides are clamped and the row SAYS so,
        # rather than the tool quietly measuring its own output format.
        # NON-FINITE VALUES ARE NOT A DIFFERENCE, they are a decode that did not
        # work. A half-float buffer read at the wrong stride is full of NaN bit
        # patterns, and np.abs(nan - x).mean() is nan -- which printed as
        # "DIFFER, mean |d| nan" and read as a finding. Counted, reported, and
        # if they dominate, the row is REFUSED rather than averaged around.
        finite = np.isfinite(t).all(axis=-1)
        bad = int((available & ~finite).sum())
        if bad:
            frac = bad / float(available.sum())
            if frac > 0.01:
                undecoded += 1
                print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} "
                      f"{w}x{h} {oi.mean():>10.4f} {'--':>11}  UNDECODED: "
                      f"{bad} of {available.sum()} available pixels are NOT FINITE"
                      f" [{100 * frac:.1f}%], so this is a decode that failed,"
                      f" not a difference")
                continue
            t = np.where(finite[..., None], t, 0.0)
        clamped = f" [{bad} non-finite pixel(s) zeroed]" if bad else ""
        if float(t[available].max()) > 1.0:
            clamped = (f" [both clamped to 0..1 for the comparison; theirs"
                       f" reaches {float(t[available].max()):.2f} and our side is an"
                       f" 8-bit PPM]")
            t = np.clip(t, 0.0, 1.0)
        note = ""
        diff = np.abs(t - oi)
        d = float(diff[available].mean())
        # A MEAN HIDES THE INTERESTING CASE. The presented buffer differs by a
        # mean of 0.025 and reads as a small difference -- and half its pixels
        # agree to 0.008 while 4.6% of them are off by more than 0.1, which is a
        # localised defect, not a global one. Both numbers, on every row: the
        # share of BADLY differing pixels is what says whether a difference is
        # a wash over the frame or a shape in one part of it.
        bad_mask = available & (diff.max(axis=-1) > 0.1)
        bad = float(bad_mask.sum() / available.sum())
        spread = f"; {100 * bad:.2f}% of available pixels differ by more than 0.1"
        if d < 0.02:
            note = "match" + spread + clamped + short
        else:
            note = f"DIFFER, mean |d| {d:.3f}{spread}{clamped}{short}"
        if d >= 0.02 or bad > 0.001:
            side = np.concatenate([oi, t], axis=1)
            Image.fromarray((np.clip(side, 0, 1) ** 0.45 * 255).astype(np.uint8)
                            ).save(out_dir / ("pass_%s%03X_%dx%d_f%d_%d.png" % k))
        print(f"  {key_str(k):>26} {our_dest:>10x} {their_dest:>11x} {w}x{h} "
              f"{oi[available].mean():>10.4f} {t[available].mean():>11.4f}  {note}")

    print(f"\n{undecoded} pass(es) were REFUSED (format not decoded here) and are "
          f"NOT counted as matching.\n{depth_pairs} DEPTH pass(es) paired on both "
          f"sides, {depth_compared} of them value-compared and "
          f"{depth_pairs - depth_compared} not (see the rows above).\n"
          f"Side-by-side images for differing passes: {out_dir}"
          f" (ours left, console right, gamma 0.45)")
    print("BLIND SPOT: this compares resolve DESTINATIONS. A pass whose output "
          "is consumed\nwithout a resolve does not appear here at all.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
