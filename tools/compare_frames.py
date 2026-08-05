#!/usr/bin/env python3
"""Compare two frames pixel for pixel, and say what the difference IS.

This is the acceptance gate for a native pass. A native pass replaces the title's
translated microcode with our own shader; the only evidence that it did so
correctly is that the frame it produces matches the frame the translated shader
produces, on the same captured input.

WHAT A NEGATIVE PRINTS. "They match" is the answer that must be hardest to get by
accident, so this tool refuses to say it cheaply:

  - two files of different size, or an unreadable one, is an ERROR, not a match,
  - two IDENTICAL files are reported as suspicious when both are blank, because a
    black frame matches a black frame and proves nothing about the shader,
  - a match prints the pixel count it is over and the frame's own mean, so
    "identical" can never be confused with "empty".

Usage: tools/compare_frames.py A.ppm B.ppm [--max-mean-diff N]
Exit 0 only when the difference is within tolerance AND the frames carry content.
"""
import sys


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    if not data.startswith(b'P6'):
        raise SystemExit(f"{path}: not a binary PPM (P6); this tool reads the "
                         f"renderer's own screenshot format")
    # Header: P6 <w> <h> <maxval>, whitespace separated, '#' comments allowed.
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    w, h, maxval = fields
    if maxval != 255:
        raise SystemExit(f"{path}: maxval {maxval}, only 8-bit PPM is supported")
    return w, h, data[i + 1:i + 1 + w * h * 3]


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    tol = 0.0
    if '--max-mean-diff' in argv:
        tol = float(argv[argv.index('--max-mean-diff') + 1])
    a_path, b_path = argv[1], argv[2]
    wa, ha, a = read_ppm(a_path)
    wb, hb, b = read_ppm(b_path)
    if (wa, ha) != (wb, hb):
        raise SystemExit(f"REFUSING to compare: {wa}x{ha} vs {wb}x{hb}. Different "
                         f"sizes are a capture mismatch, not a shading difference.")
    n = len(a)
    if n == 0 or len(b) != n:
        raise SystemExit(f"REFUSING to compare: {n} vs {len(b)} pixel bytes")

    total = diffsum = 0
    worst = 0
    mean_a = sum(a) / n
    mean_b = sum(b) / n
    hist = [0] * 5   # 0, 1, 2-4, 5-16, >16
    for x, y in zip(a, b):
        d = x - y if x > y else y - x
        diffsum += d
        total += 1
        if d > worst:
            worst = d
        hist[0 if d == 0 else 1 if d == 1 else 2 if d < 5 else 3 if d < 17 else 4] += 1
    mean_diff = diffsum / total

    print(f"{a_path}: {wa}x{ha}, mean channel value {mean_a:.3f}")
    print(f"{b_path}: {wb}x{hb}, mean channel value {mean_b:.3f}")
    print(f"mean |difference| {mean_diff:.4f} of 255, worst channel {worst}")
    print(f"  exact {hist[0]}  off-by-1 {hist[1]}  2-4 {hist[2]}"
          f"  5-16 {hist[3]}  >16 {hist[4]}   (of {total} channel samples)")

    # A match between two blank frames is not evidence of anything.
    if max(mean_a, mean_b) < 1.0:
        print("INCONCLUSIVE: both frames are essentially black. A native pass that "
              "draws nothing matches a translated pass that draws nothing; this "
              "comparison cannot tell them apart. Capture a frame with content.")
        return 2
    if mean_diff > tol:
        print(f"DIFFERENT: mean |difference| {mean_diff:.4f} exceeds tolerance {tol}")
        return 1
    print(f"MATCH within {tol}: over {total} channel samples of a frame whose own "
          f"mean is {mean_a:.3f}, so this is a match on content, not on emptiness.")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
