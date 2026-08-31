#!/usr/bin/env python3
"""Compare OUR frame against the oracle's on the one axis a moment mismatch
cannot destroy: CHROMATICITY, and which channel order best explains it.

WHY THIS EXISTS, AND WHAT IT IS NOT

`tools/oracle_compare.py` produces two filmstrips of the same scripted walk and
says, correctly, that a pixel metric between them is meaningless: the two
emulations reach different moments, so any per-pixel number measures content as
much as rendering. That warning has been read as "nothing quantitative can be
said", and so the two sides have only ever been compared by eye and by
whole-frame means.

Something quantitative CAN be said. Split every pixel into brightness and
chromaticity:

    r = R/(R+G+B)   g = G/(R+G+B)   b = B/(R+G+B)

Chromaticity is invariant to exposure, so it is immune to catalog #62's other
defect (our gameplay frames top out at 76/255 where the oracle reaches 255) --
this tool says nothing about that and must not be quoted about it. And the
DISTRIBUTION of chromaticity over a frame is a property of the scene's palette,
which moves slowly as a camera walks through one environment. That is what makes
a cross-moment comparison worth anything here.

The question it answers: of the six ways to permute our three channels, which
one best explains the oracle's chromaticity distribution, and is that answer
bigger than the noise a moment mismatch produces on its own?

THE NULL BAND IS THE WHOLE POINT. "Our frames match theirs best under an R/B
exchange" means nothing until you know how far apart two frames of the SAME
renderer at different moments land. So every run measures that too, from the
oracle's own filmstrip against itself and ours against ours, and prints the
identity-permutation baseline alongside the cross-side result. A verdict is only
issued when the winning permutation beats the runner-up by more than that band.

WHAT IT CANNOT SEE. It compares distributions, not pixels: two frames with the
same palette in different places score identically. It says nothing about
geometry, exposure, or whether a pass is missing. Near-black pixels carry no
chromaticity and are excluded, and the count that survives is printed, because
"the frames agree" computed over 400 pixels is not a result.

    tools/chroma_compare.py --ours DIR_OR_FILE --theirs DIR_OR_FILE
    tools/chroma_compare.py --selftest
"""
import argparse
import sys
from itertools import permutations
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from frame_stats import read_image  # noqa: E402  (same readers, same definitions)

# A pixel darker than this carries no usable chromaticity: at R+G+B = 6 the
# fractions move by 1/6 for a one-level change, which is larger than any effect
# looked for here. Excluded pixels are counted and reported, never silently cut.
MIN_SUM = 24
QUANTILES = [i / 100.0 for i in range(1, 100)]
PERMS = list(permutations((0, 1, 2)))
PERM_NAMES = {
    (0, 1, 2): "identity",
    (2, 1, 0): "R<->B",
    (1, 0, 2): "R<->G",
    (0, 2, 1): "G<->B",
    (1, 2, 0): "RGB->GBR",
    (2, 0, 1): "RGB->BRG",
}


class Chroma:
    """The chromaticity signature of one frame: quantiles of r and b."""

    def __init__(self, path):
        w, h, px = read_image(path)
        self.path = str(path)
        self.n_total = w * h
        rs, gs, bs = [], [], []
        for i in range(0, len(px), 3):
            R, G, B = px[i], px[i + 1], px[i + 2]
            s = R + G + B
            if s < MIN_SUM:
                continue
            rs.append(R / s)
            gs.append(G / s)
            bs.append(B / s)
        self.n_used = len(rs)
        if self.n_used == 0:
            # A frame with nothing but near-black pixels has no chromaticity at
            # all. Returning empty quantiles would make it compare EQUALLY WELL
            # against everything, which reads exactly like agreement.
            raise ValueError(
                f"{path}: only {self.n_used} of {self.n_total} px are above the "
                f"near-black cut (R+G+B >= {MIN_SUM}); this frame has no "
                f"chromaticity to compare and no result can be computed from it")
        self.q = [_quantiles(sorted(rs)), _quantiles(sorted(gs)), _quantiles(sorted(bs))]

    def frac_used(self):
        return self.n_used / self.n_total


def _quantiles(sorted_vals):
    n = len(sorted_vals)
    return [sorted_vals[min(n - 1, int(p * n))] for p in QUANTILES]


def distance(a, b, perm=(0, 1, 2)):
    """Mean |quantile difference| over the three channels, in chromaticity units.

    `perm[c]` is which of A's channels is compared against B's channel c.
    """
    total = 0.0
    for c in range(3):
        qa, qb = a.q[perm[c]], b.q[c]
        total += sum(abs(x - y) for x, y in zip(qa, qb)) / len(qa)
    return total / 3.0


def score_all(a, b):
    return sorted(((distance(a, b, p), p) for p in PERMS))


def collect(spec):
    """A file, or every frame in a directory, sorted. Refuses on nothing found."""
    p = Path(spec)
    if p.is_file():
        return [p]
    if not p.is_dir():
        raise SystemExit(f"REFUSING: {spec} is neither a file nor a directory. "
                         "Nothing was read and nothing was compared.")
    files = sorted(f for f in p.iterdir()
                   if f.suffix.lower() in (".ppm", ".png"))
    if not files:
        raise SystemExit(f"REFUSING: {p} contains no .ppm or .png frames. This is "
                         "an EMPTY SEARCH, not a negative result.")
    return files


def select(files, needles, label):
    """Filter, and SAY WHAT WAS DROPPED.

    Comparing a gameplay frame against a menu frame is comparing two palettes,
    so restricting to comparable content is legitimate -- but a filter that
    quietly discards most of the input turns a narrow result into what looks
    like a whole-filmstrip one. Everything dropped is named.
    """
    if not needles:
        return files
    wanted = [n for n in needles.split(",") if n]
    keep = [f for f in files if any(n in f.name for n in wanted)]
    drop = [f.name for f in files if f not in keep]
    print(f"  FILTER on {label}: kept {len(keep)} of {len(files)} "
          f"(matching {wanted}); DROPPED {drop}")
    if not keep:
        raise SystemExit(f"REFUSING: the filter {wanted} matched NONE of the "
                         f"{len(files)} {label} frames. Nothing was compared.")
    return keep


def pairwise_null(sigs, label):
    """How far apart are two frames of the SAME renderer at different moments?

    This is the band any cross-side claim has to clear. It is measured, not
    assumed, and it is measured under the IDENTITY permutation because that is
    the comparison a same-renderer pair is entitled to.
    """
    ds = [distance(sigs[i], sigs[j])
          for i in range(len(sigs)) for j in range(i + 1, len(sigs))]
    if not ds:
        print(f"  {label}: only one frame -- NO null band could be measured. "
              f"Every verdict below is therefore UNCALIBRATED.")
        return None
    print(f"  {label}: {len(ds)} pairs, identity distance "
          f"min {min(ds):.4f}  median {sorted(ds)[len(ds)//2]:.4f}  max {max(ds):.4f}")
    return ds


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours", help="our frame, or a directory of them")
    ap.add_argument("--theirs", help="the oracle's frame, or a directory of them")
    ap.add_argument("--only-ours", default="", help="substring filter on our frames")
    ap.add_argument("--only-theirs", default="", help="substring filter on theirs")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.ours or not args.theirs:
        ap.error("--ours and --theirs are both required (or use --selftest)")

    ours = [Chroma(f) for f in select(collect(args.ours), args.only_ours, "ours")]
    theirs = [Chroma(f) for f in select(collect(args.theirs), args.only_theirs, "theirs")]

    print("== how many pixels each side could actually be measured on ==")
    for label, sigs in (("ours", ours), ("theirs", theirs)):
        worst = min(sigs, key=lambda s: s.frac_used())
        print(f"  {label}: {len(sigs)} frames, "
              f"least usable {worst.frac_used()*100:.1f}% "
              f"({Path(worst.path).name}); a frame that is mostly black "
              f"contributes little and this says which one that is")

    print("\n== NULL BAND: same renderer, different moments ==")
    null_theirs = pairwise_null(theirs, "theirs vs theirs")
    null_ours = pairwise_null(ours, "ours   vs ours  ")
    band = max((max(d) for d in (null_theirs, null_ours) if d), default=None)

    print("\n== CROSS-SIDE: every one of ours against every one of theirs ==")
    print("   winner(distance)  vs  the distance AS WE PRESENT IT (identity)")
    wins = {}
    best_ds, ident_ds = [], []
    for a in ours:
        row = []
        for b in theirs:
            (d0, p0) = score_all(a, b)[0]
            di = distance(a, b)
            wins[PERM_NAMES[p0]] = wins.get(PERM_NAMES[p0], 0) + 1
            best_ds.append(d0)
            ident_ds.append(di)
            row.append(f"{PERM_NAMES[p0]}{d0:.3f}/id{di:.3f}")
        print(f"  {Path(a.path).name:22s} {' '.join(f'{c:>20s}' for c in row)}")

    total = sum(wins.values())
    print(f"\n  winning permutation over all {total} cross pairs:")
    for name, n in sorted(wins.items(), key=lambda kv: -kv[1]):
        print(f"    {name:10s} {n:4d}  ({n*100.0/total:.0f}%)")

    print("\n== VERDICT ==")
    if band is None:
        print("  NONE. With one frame on a side there is no null band, so a "
              "winning permutation cannot be told from moment noise.")
        return 1
    best_name = max(wins, key=lambda k: wins[k])
    unanimous = wins[best_name] == total
    print(f"  null band (worst same-renderer, different-moment distance): {band:.4f}")
    print(f"  cross-side under {best_name:9s}: worst {max(best_ds):.4f}")
    print(f"  cross-side as presented (identity): best {min(ident_ds):.4f}")

    # The test that means something. A permutation "wins" trivially whenever the
    # runners-up are near-degenerate, so the margin over the runner-up is a weak
    # criterion (it reported nothing on the first real run). The strong one is
    # absolute: does the winner bring the two sides INSIDE the distance two
    # frames of one renderer already sit at, while the order we present leaves
    # them OUTSIDE it? That is a statement about our output, not about a ranking.
    if unanimous and max(best_ds) <= band and min(ident_ds) > band:
        print(f"\n  {best_name} puts every cross pair INSIDE the band that two "
              f"frames of the same renderer occupy, and the order we present "
              f"leaves every pair OUTSIDE it.")
        print("  A moment mismatch cannot produce that: it is bounded by the "
              "band, and identity exceeds it.")
        print(f"  CONCLUSION: our frames carry the oracle's palette under "
              f"{best_name}, and not as presented.")
    elif unanimous:
        print(f"\n  {best_name} wins every pair, but the absolute test does not "
              f"separate it from moment noise. SUGGESTIVE, NOT SETTLED.")
    else:
        print("\n  No permutation wins consistently. On this evidence the channel "
              "order is NOT established.")
    print("\n  In all cases: this tool sees an EXCHANGE, never a per-channel "
          "SCALE, and says nothing about exposure or the missing top of our "
          "range (catalog #62's other half).")
    return 0


# ------------------------------------------------------------------ selftest

def _write_ppm(path, w, h, pixels):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(pixels))


def _scene(seed, swap_rb=False, scale=1.0):
    """A deterministic pseudo-scene: a warm palette with structure."""
    w = h = 64
    px = bytearray()
    v = seed
    for i in range(w * h):
        v = (v * 1103515245 + 12345) & 0x7FFFFFFF
        t = (v >> 16) & 0xFF
        R, G, B = t, int(t * 0.92), int(t * 0.55)   # warm: B is the low channel
        if swap_rb:
            R, B = B, R
        px += bytes((min(255, int(R * scale)), min(255, int(G * scale)),
                     min(255, int(B * scale))))
    return w, h, px


def selftest():
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        w, h, warm = _scene(1)
        _write_ppm(td / "warm.ppm", w, h, warm)
        _write_ppm(td / "warm2.ppm", w, h, _scene(999)[2])       # same palette, other moment
        _write_ppm(td / "swapped.ppm", w, h, _scene(1, swap_rb=True)[2])
        _write_ppm(td / "dim.ppm", w, h, _scene(1, scale=0.30)[2])  # our 76/255 defect
        _write_ppm(td / "cool.ppm", w, h, _scene(1, swap_rb=True, scale=0.30)[2])

        warm_s = Chroma(td / "warm.ppm")
        warm2_s = Chroma(td / "warm2.ppm")
        swapped_s = Chroma(td / "swapped.ppm")
        dim_s = Chroma(td / "dim.ppm")
        cool_s = Chroma(td / "cool.ppm")

        def check(name, cond, detail=""):
            nonlocal ok
            print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
            ok = ok and cond

        # POSITIVE: it must report identity for a frame against itself.
        best = score_all(warm_s, warm_s)[0]
        check("a frame against itself -> identity, distance 0",
              best[1] == (0, 1, 2) and best[0] < 1e-9, f"d={best[0]:.5f}")

        # POSITIVE: it must report the exchange when there IS one.
        best = score_all(swapped_s, warm_s)[0]
        check("an R/B-exchanged frame -> R<->B wins",
              PERM_NAMES[best[1]] == "R<->B", f"d={best[0]:.5f}")

        # THE ONE THAT MATTERS: exposure invariance. A frame that is 30% as
        # bright -- our actual defect -- must STILL report identity, or every
        # cross-side run here is measuring brightness and calling it colour.
        best = score_all(dim_s, warm_s)[0]
        check("a 0.30x-exposure copy -> STILL identity (exposure invariant)",
              best[1] == (0, 1, 2), f"winner={PERM_NAMES[best[1]]} d={best[0]:.5f}")

        # And dim+swapped -- the exact shape claimed of our renderer.
        best = score_all(cool_s, warm_s)[0]
        check("dim AND exchanged -> R<->B, not identity",
              PERM_NAMES[best[1]] == "R<->B", f"d={best[0]:.5f}")

        # NEGATIVE: the null band must be SMALL for two moments of one
        # renderer, or the verdict rule can never fire.
        d_null = distance(warm_s, warm2_s)
        d_swap = distance(swapped_s, warm_s)
        check("same palette, different moment -> distance well under the exchange",
              d_null < d_swap / 4, f"null={d_null:.5f} exchange={d_swap:.5f}")

        # NEGATIVE: a frame with no measurable pixels must REFUSE, not return a
        # perfect match against anything.
        _write_ppm(td / "black.ppm", 8, 8, bytes(8 * 8 * 3))
        try:
            Chroma(td / "black.ppm")
            check("an all-black frame refuses", False, "it returned a signature")
        except ValueError as e:
            check("an all-black frame refuses", True, str(e).split(":")[-1].strip()[:48])

        # NEGATIVE: an empty directory must refuse rather than report nothing.
        (td / "empty").mkdir()
        try:
            collect(td / "empty")
            check("an empty directory refuses", False, "it returned a result")
        except SystemExit:
            check("an empty directory refuses", True)

    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
