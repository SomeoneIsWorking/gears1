#!/usr/bin/env python3
"""Compare a runtime XMA decode against the golden ffmpeg reference.

Usage: xma_compare.py <golden.wav> <candidate.wav> [--seconds N] [--report-lag-only]

The two decodes of the same stream differ legitimately in one way: alignment.
ffmpeg's file-level xma2 decoder trims encoder priming samples; the hardware
protocol (and therefore the runtime) emits every decoded sample. So the
comparison first finds the lag by cross-correlation, then measures how well
the overlapping region matches. It prints the lag, the correlation, and the
RMS/peak of the difference, and exits nonzero when the correlation says the
two are not the same audio.

A near-1.0 correlation with a small constant lag is a pass. A high RMS with a
wandering best-lag is not "close" -- it is a decode bug, and this tool will
not call it a pass.
"""

import argparse
import sys
import wave

import numpy as np


def load(path):
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        channels = w.getnchannels()
        assert w.getsampwidth() == 2, f"{path}: not 16-bit PCM"
        frames = w.readframes(w.getnframes())
    data = np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0
    data = data.reshape(-1, channels)
    return rate, channels, data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("golden")
    parser.add_argument("candidate")
    parser.add_argument("--seconds", type=float, default=20.0,
                        help="length of the comparison window")
    parser.add_argument("--max-lag", type=float, default=1.0,
                        help="maximum lag searched, in seconds")
    args = parser.parse_args()

    grate, gch, golden = load(args.golden)
    crate, cch, candidate = load(args.candidate)
    if grate != crate or gch != cch:
        print(f"FAIL: format mismatch: golden {grate} Hz x{gch}, "
              f"candidate {crate} Hz x{cch}")
        return 1

    window = int(args.seconds * grate)
    max_lag = int(args.max_lag * grate)
    g = golden[:window].mean(axis=1)
    c = candidate[:window + max_lag].mean(axis=1)
    if len(g) < grate or len(c) < grate:
        print(f"FAIL: less than a second to compare "
              f"(golden {len(g)}, candidate {len(c)} samples)")
        return 1

    # Cross-correlate to find the candidate's lead over the golden (the
    # candidate is expected to LEAD: it contains priming samples ffmpeg trims).
    n = len(g) + len(c)
    fg = np.fft.rfft(g, n)
    fc = np.fft.rfft(c, n)
    corr = np.fft.irfft(fc * np.conj(fg), n)
    lags = np.arange(-len(g) + 1, len(c))
    corr = np.roll(corr, len(g) - 1)[: len(lags)]
    keep = (lags >= -max_lag) & (lags <= max_lag)
    lag = int(lags[keep][np.argmax(corr[keep])])

    # The FFT peak is an estimate; a few samples off is the difference
    # between correlation 1.0 and 0.7 on music. Refine by direct normalized
    # correlation in a window around it and trust that, not the estimate.
    def normalized(shift):
        if shift >= 0:
            a2, b2 = g[: len(g) - shift or None], c[shift:shift + len(g)]
        else:
            a2, b2 = g[-shift:], c[: len(g) + shift]
        m2 = min(len(a2), len(b2))
        if m2 < 1024:
            return -1.0
        a2, b2 = a2[:m2], b2[:m2]
        d = np.sqrt((a2 * a2).sum() * (b2 * b2).sum())
        return float((a2 * b2).sum() / d) if d else -1.0

    lag = max(range(lag - 32, lag + 33), key=normalized)

    if lag >= 0:
        a, b = golden[:window], candidate[lag:lag + window]
    else:
        a, b = golden[-lag:window], candidate[:window + lag]
    m = min(len(a), len(b))
    a, b = a[:m], b[:m]

    denominator = np.sqrt((a * a).sum() * (b * b).sum())
    correlation = float((a * b).sum() / denominator) if denominator else 0.0
    diff = a - b
    rms = float(np.sqrt((diff * diff).mean()))
    peak = float(np.abs(diff).max())

    print(f"lag: {lag} samples ({lag / grate * 1000:.1f} ms, candidate "
          f"{'leads' if lag >= 0 else 'trails'})")
    print(f"compared: {m} frames ({m / grate:.2f} s) at {grate} Hz x{gch}")
    print(f"correlation: {correlation:.6f}")
    print(f"difference: rms {rms:.6f}, peak {peak:.6f} (full scale = 1.0)")

    if correlation >= 0.999:
        print("PASS: same audio")
        return 0
    if correlation >= 0.99:
        print("MARGINAL: same audio with measurable differences -- "
              "investigate before calling this done")
        return 0
    print("FAIL: these are not the same decode")
    return 1


if __name__ == "__main__":
    sys.exit(main())
