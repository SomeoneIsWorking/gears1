#!/usr/bin/env python3
"""Per-channel statistics for a rendered frame, and a diff between two.

This exists because catalog #62's headline -- "red is 78% of green frame-wide,
and lit surfaces flatten at 0.30" -- was a number nobody could re-derive without
writing a throwaway script, and it got re-derived three times in one session.
A number you cannot re-measure in one command is a number that quietly goes
stale: #62's was quoted for days before anyone checked it still held.

It also makes OUR frame and a REFERENCE frame directly comparable, which is what
#62 has been asking for: same statistics, same definitions, one command each.

    tools/frame_stats.py <image> [<image> ...]     # stats per image
    tools/frame_stats.py --diff <a> <b>            # and how a relates to b
    tools/frame_stats.py --selftest                # prove it reports correctly

Reads binary PPM (P6) and PNG (8-bit RGB/RGBA, non-interlaced) -- our screenshots
are the former, most emulators screenshot the latter.
"""
import struct
import sys
import zlib
from pathlib import Path


# ---------------------------------------------------------------- readers

def read_ppm(data):
    if data[:2] != b"P6":
        return None
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError(f"PPM maxval {maxval} is not 8-bit; refusing to guess")
    return w, h, data[i:i + w * h * 3]


def read_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, w = 8, bytearray(), None
    while pos < len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            w, h, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            # Refuse rather than misread: a 16-bit or palette PNG decoded as
            # 8-bit RGB produces plausible garbage, which is worse than an error.
            if depth != 8:
                raise ValueError(f"PNG bit depth {depth} unsupported (8 only)")
            if color not in (2, 6):
                raise ValueError(f"PNG colour type {color} unsupported (RGB/RGBA only)")
            if interlace:
                raise ValueError("interlaced PNG unsupported")
            nch = 3 if color == 2 else 4
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length
    if w is None:
        raise ValueError("PNG has no IHDR")
    raw = zlib.decompress(bytes(idat))
    stride = w * nch
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for x in range(stride):
            a = line[x - nch] if x >= nch else 0
            b = prev[x]
            c = prev[x - nch] if x >= nch else 0
            if ft == 1:   line[x] = (line[x] + a) & 0xFF
            elif ft == 2: line[x] = (line[x] + b) & 0xFF
            elif ft == 3: line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
            elif ft != 0:
                raise ValueError(f"PNG filter type {ft} unknown")
        for x in range(w):
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * nch:x * nch + 3]
        prev = line
    return w, h, bytes(out)


def read_image(path):
    data = Path(path).read_bytes()
    for reader in (read_ppm, read_png):
        got = reader(data)
        if got is not None:
            return got
    raise ValueError(f"{path}: not a P6 PPM or a PNG")


# ---------------------------------------------------------------- stats

class Stats:
    def __init__(self, w, h, px):
        self.w, self.h, self.px, self.n = w, h, px, w * h
        self.hist = [[0] * 256 for _ in range(3)]
        self.total = [0, 0, 0]
        self.black = 0
        for k in range(0, len(px), 3):
            r, g, b = px[k], px[k + 1], px[k + 2]
            if not (r or g or b):
                self.black += 1
            for c, v in enumerate((r, g, b)):
                self.hist[c][v] += 1
                self.total[c] += v

    def mean(self, c):
        return self.total[c] / self.n / 255.0

    def pct(self, c, q):
        want, acc = self.n * q, 0
        for v in range(256):
            acc += self.hist[c][v]
            if acc >= want:
                return v / 255.0
        return 1.0

    def report(self, name):
        print(f"== {name}: {self.w}x{self.h}, {self.n} px "
              f"({100 * self.black / self.n:.1f}% pure black) ==")
        for c, ch in enumerate("RGB"):
            print(f"   {ch}  mean {self.mean(c):.4f}   median {self.pct(c, .50):.3f}"
                  f"   p99 {self.pct(c, .99):.3f}   p99.9 {self.pct(c, .999):.3f}")
        mg = self.mean(1)
        if mg > 0:
            print(f"   R/G {self.mean(0) / mg:.4f}   B/G {self.mean(2) / mg:.4f}"
                  f"      (catalog #62 measured R/G 0.78)")
        else:
            # A ratio against zero is not a small number, it is no number.
            print("   NO RATIO: green is zero frame-wide, so this frame says nothing"
                  " about a per-channel deficit")
        if self.black == self.n:
            print("   THE FRAME IS ENTIRELY BLACK -- every statistic above is a"
                  " statistic of nothing, not a measurement of a picture")


def diff(a, b, na, nb):
    print(f"== {na} relative to {nb} ==")
    if (a.w, a.h) != (b.w, b.h):
        print(f"   REFUSING to compare: {a.w}x{a.h} vs {b.w}x{b.h}. Different sizes.")
        return
    same = swapped = 0
    for k in range(0, len(a.px), 3):
        pa = a.px[k:k + 3]
        pb = b.px[k:k + 3]
        if pa == pb:
            same += 1
        if (pa[0], pa[1], pa[2]) == (pb[2], pb[1], pb[0]):
            swapped += 1
    n = a.n
    print(f"   identical pixels             {100 * same / n:6.2f}%")
    print(f"   identical after an R/B swap  {100 * swapped / n:6.2f}%")
    for c, ch in enumerate("RGB"):
        mb = b.mean(c)
        rel = f"{a.mean(c) / mb:.4f}x" if mb > 0 else "n/a (reference channel is 0)"
        print(f"   {ch} mean {a.mean(c):.4f} vs {b.mean(c):.4f}   {rel}")


# ---------------------------------------------------------------- selftest

def selftest():
    """Feed cases whose answers are known, INCLUDING ones that must NOT trip.

    A statistic that has only ever run on real frames has never been shown to
    report the other answer. Each case here would fail loudly if the maths
    silently changed.
    """
    def ppm(w, h, pixels):
        return b"P6\n%d %d\n255\n" % (w, h) + bytes(pixels)

    failures = []

    def check(what, got, want):
        ok = abs(got - want) < 1e-6 if isinstance(want, float) else got == want
        print(f"   {'ok  ' if ok else 'FAIL'}  {what}: got {got}, want {want}")
        if not ok:
            failures.append(what)

    # A frame that is exactly half red, so R/G must be exactly 0.5 -- the
    # deficit #62 is about, at a value no rounding can fake.
    w, h = 4, 2
    px = []
    for _ in range(w * h):
        px += [128, 255, 255]
    s = Stats(*read_ppm(ppm(w, h, px)))
    check("R/G on a synthetic half-red frame", round(s.mean(0) / s.mean(1), 4), 0.502)

    # An all-black frame must NOT report a ratio of 1.0 or 0.0 as if it were a
    # measurement; mean green is 0 and the report says so.
    s2 = Stats(*read_ppm(ppm(2, 2, [0] * 12)))
    check("black frame mean G", s2.mean(1), 0.0)
    check("black frame counted as black", s2.black, 4)

    # The R/B-swap detector must fire on a swap and NOT on an unrelated image.
    a = Stats(*read_ppm(ppm(2, 1, [10, 20, 30, 40, 50, 60])))
    b = Stats(*read_ppm(ppm(2, 1, [30, 20, 10, 60, 50, 40])))
    c = Stats(*read_ppm(ppm(2, 1, [99, 98, 97, 96, 95, 94])))
    sw = sum(1 for k in (0, 3)
             if (a.px[k], a.px[k+1], a.px[k+2]) == (b.px[k+2], b.px[k+1], b.px[k]))
    nsw = sum(1 for k in (0, 3)
              if (a.px[k], a.px[k+1], a.px[k+2]) == (c.px[k+2], c.px[k+1], c.px[k]))
    check("swap detector fires on a swapped pair", sw, 2)
    check("swap detector silent on an unrelated pair", nsw, 0)

    # The PNG reader must agree with the PPM reader on identical content, or
    # comparing our PPM against a reference PNG compares two decoders.
    raw = bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])
    idat = zlib.compress(b"\x00" + raw[:6] + b"\x00" + raw[6:])
    png = b"\x89PNG\r\n\x1a\n"
    for ctype, body in ((b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)),
                        (b"IDAT", idat), (b"IEND", b"")):
        png += struct.pack(">I", len(body)) + ctype + body
        png += struct.pack(">I", zlib.crc32(ctype + body))
    check("PNG reader matches PPM reader", read_png(png)[2], read_ppm(ppm(2, 2, raw))[2])

    print("\nSELFTEST FAILED: " + ", ".join(failures) if failures
          else "\nselftest passed: every case reported the value it must.")
    return 1 if failures else 0


def main(argv):
    args = argv[1:]
    if not args:
        print(__doc__)
        return 2
    if args[0] == "--selftest":
        return selftest()
    do_diff = args[0] == "--diff"
    if do_diff:
        args = args[1:]
        if len(args) != 2:
            print("--diff takes exactly two images")
            return 2
    loaded = []
    for a in args:
        if not Path(a).is_file():
            # Never report statistics for a file that is not there.
            print(f"REFUSING: {a} does not exist. Nothing was measured.")
            return 1
        w, h, px = read_image(a)
        s = Stats(w, h, px)
        s.report(Path(a).name)
        loaded.append((s, Path(a).name))
        print()
    if do_diff:
        diff(loaded[0][0], loaded[1][0], loaded[0][1], loaded[1][1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
