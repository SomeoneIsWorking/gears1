#!/usr/bin/env python3
"""Pair every resolve of one captured frame across OUR renderer and the oracle.

    tools/resolve_pair.py <capture.gfr> <ours.log> <oracle.log>

`ours.log`   a frame_replay run with GEARS_DRAW_RESOLVE_DUMP_EACH=1
`oracle.log` a xenia-gpu-vulkan-trace-dump run of the SAME capture's trace with
             GEARS_PROBE_AFTER_RESOLVE=1

WHY THIS IS A TOOL AND NOT A ONE-OFF. The two sides number their resolves
differently and the mismatch is silent. Our renderer executes 14 of bright.gfr's
18 copy draws -- depth resolves take another path and the untile collapse drops
a tile's worth -- so "resolve 3" is a DIFFERENT resolve on each side, and lining
the two lists up by position produces a table that looks authoritative and
compares unrelated buffers. The capture's own copy draws are the only shared
index: the oracle's Nth IssueCopy is the capture's Nth copy draw, and our dump
names its draw index in the filename. This pairs by that.

WHAT THE NUMBERS ARE, because they are NOT the same measurement. The oracle
counts non-zero BYTES over the tiled destination in shared memory; ours counts
non-zero COMPONENTS over the untiled RGB image. Only gross agreement or
disagreement means anything -- the threshold is deliberately coarse and is
printed. A few points apart is noise between two different metrics.

AND THE ORACLE'S LATE RESOLVES ARE KNOWN BROKEN (catalog #79): in trace playback
its early resolves land and its late ones write zeros. A row where the oracle
reads ~0 and we read a full buffer is that defect, not a finding about us, and
those rows are labelled rather than counted as divergences.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gfr_to_xtr import Capture, all_resolves          # noqa: E402

# Two different metrics; anything inside this is not a signal.
AGREE_PCT = 10.0
# Below this the oracle's destination is empty, which for a LATE resolve is
# catalog #79's playback defect rather than a statement about our renderer.
ORACLE_EMPTY_PCT = 2.0

ORACLE_RE = re.compile(
    r'after-resolve swap #(\d+)\]: ([0-9A-F]{8}) \((\d+) bytes, tiled\): '
    r'(\d+) non-zero \(([\d.]+)%\), mean ([\d.]+)')
OURS_RE = re.compile(
    r'resolve_(\d+)_([0-9a-f]{8})_draw(\d+)\.ppm \(range ([-\d.]+) \.\. '
    r'([\d.]+), (\d+) of (\d+) components non-zero \[([\d.]+)%\]')


def parse_oracle(path):
    out = []
    for line in path.read_text(errors="replace").splitlines():
        m = ORACLE_RE.search(line)
        if m:
            out.append(dict(n=int(m[1]), dest=m[2], pct=float(m[5]),
                            mean=float(m[6])))
    return out


def parse_ours(path):
    out = {}
    for line in path.read_text(errors="replace").splitlines():
        m = OURS_RE.search(line)
        if m:
            out[int(m[3])] = dict(dest=m[2], pct=float(m[8]), hi=float(m[5]))
    return out


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    cap_path, ours_path, oracle_path = (Path(a) for a in argv[1:])
    for p in (cap_path, ours_path, oracle_path):
        if not p.is_file():
            # REFUSE rather than report an empty table. A missing log and a run
            # in which nothing resolved read identically otherwise.
            print(f"REFUSING: {p} does not exist. Nothing was compared.")
            return 1

    resolves = all_resolves(Capture(cap_path))
    oracle = parse_oracle(oracle_path)
    ours = parse_ours(ours_path)

    if not oracle:
        print(f"REFUSING: {oracle_path} contains no after-resolve probe lines. "
              f"Re-run the trace dump with GEARS_PROBE_AFTER_RESOLVE=1 and "
              f"--gears_probe_front_buffer=1. Nothing was compared.")
        return 1
    if not ours:
        print(f"REFUSING: {ours_path} contains no per-resolve dump lines. "
              f"Re-run frame_replay with GEARS_DRAW_RESOLVE_DUMP_EACH=1. "
              f"Nothing was compared.")
        return 1
    if len(oracle) != len(resolves):
        print(f"REFUSING: the capture has {len(resolves)} copy draws but the "
              f"oracle log has {len(oracle)} resolves. They are not the same "
              f"frame, or the trace was truncated (--draws / --present "
              f"resolve:N). Pairing them by position would compare unrelated "
              f"buffers. Nothing was compared.")
        return 1

    print(f"{'orc#':>4} {'draw':>5} {'dest':>9} {'kind':>6} | "
          f"{'oracle':>8} {'mean':>6} | {'ours':>8} {'max':>7} | verdict")
    counts = {}
    for o in oracle:
        r = resolves[o["n"]]
        if f"{r['base']:08X}"[-8:] != o["dest"]:
            print(f"REFUSING at resolve {o['n']}: the capture says destination "
                  f"{r['base']:#010x} and the oracle says {o['dest']}. The two "
                  f"logs are of different frames.")
            return 1
        kind = "depth" if r["depth"] else "color"
        u = ours.get(r["draw"])
        if u is None:
            # A DEPTH ROW IS A BLIND SPOT, NOT A GAP. Our renderer reports
            # "frame depth resolves: 0 executed" and still has content at those
            # destinations -- the deferred passes sample them as k_24_8_FLOAT
            # through a decode path that never calls ResolveSurfaceTo, which is
            # the only place the per-resolve dump hooks. Labelling these the
            # same as a colour resolve we genuinely skip would read as a missing
            # resolve, and it is not one.
            v = ("depth -- served by decode, not by the dumped path: THIS TOOL "
                 "CANNOT SEE IT" if r["depth"]
                 else "not executed by our renderer")
            print(f"{o['n']:>4} {r['draw']:>5} {o['dest']:>9} {kind:>6} | "
                  f"{o['pct']:>7.1f}% {o['mean']:>6.1f} | {'--':>8} {'--':>7} "
                  f"| {v}")
            counts[v] = counts.get(v, 0) + 1
            continue
        d = o["pct"] - u["pct"]
        if abs(d) < AGREE_PCT:
            v = "agree"
        elif d < 0 and o["pct"] < ORACLE_EMPTY_PCT:
            v = "oracle empty -- #79 playback defect, says nothing about us"
        elif d < 0:
            v = "WE HAVE MORE"
        else:
            v = "ORACLE HAS MORE"
        counts[v] = counts.get(v, 0) + 1
        print(f"{o['n']:>4} {r['draw']:>5} {o['dest']:>9} {kind:>6} | "
              f"{o['pct']:>7.1f}% {o['mean']:>6.1f} | {u['pct']:>7.1f}% "
              f"{u['hi']:>7.3f} | {v}")

    print()
    for k, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {n:>3}  {k}")
    print(f"\n{len(resolves)} copy draws; agreement threshold {AGREE_PCT}%.")
    print("The two percentages are DIFFERENT metrics -- the oracle counts "
          "non-zero BYTES over\nthe tiled destination, ours counts non-zero "
          "COMPONENTS over the untiled RGB image.\nOnly gross "
          "agreement/disagreement is meaningful.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
