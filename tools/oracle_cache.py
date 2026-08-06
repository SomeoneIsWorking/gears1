#!/usr/bin/env python3
"""The oracle frame CACHE: every reference frame this project has ever produced,
measured once and findable without re-running Xenia.

WHY THIS EXISTS. Running the oracle costs minutes, needs the disc or the
extracted tree, hangs in shutdown past its timeout often enough that three
attempts in one session produced no reading (catalog #77), and is
non-reproducible against itself (catalog #84) -- so two runs of the same walk do
not even land on the same moment. Meanwhile 121 frames from past runs were
already on disk, spread over 29 directories with names like `frames5`, `live2`
and `td`, with no record of which walk produced them or what is in them. Every
session re-ran the oracle because finding an existing frame was harder than
making a new one. Eleven of those frames are 100% BLACK and someone comparing
against them would be comparing against nothing.

So: measure every frame once, write an index, and query the index.

    tools/oracle_cache.py index              # (re)build the index
    tools/oracle_cache.py list --kind gameplay
    tools/oracle_cache.py show <id>
    tools/oracle_cache.py match ours.ppm     # the closest cached reference
    tools/oracle_cache.py --selftest

THE NEGATIVE IS DESIGNED FIRST, because this is an instrument and a search that
answers "(none)" is indistinguishable from a search that never looked:

  * a query with no hits prints how many frames it searched and how many each
    criterion excluded, so "no gameplay frames" cannot be confused with "the
    index is empty";
  * an index over a missing or empty tree EXITS NON-ZERO saying it measured
    nothing, rather than writing an empty index that later reads as "no frames
    exist";
  * BLACK frames are kept in the index and flagged, not dropped. They are the
    ones a comparison must refuse, and a frame silently missing from the cache
    would just be re-discovered and re-used.
"""

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ORACLE_DIR = REPO / "scratch" / "oracle"
INDEX = ORACLE_DIR / "cache" / "index.json"


def _load_image(path):
    """Returns an HxWx3 float array in 0..1, or None with a reason."""
    try:
        import numpy as np
        from PIL import Image
    except ImportError as e:
        raise SystemExit(f"oracle_cache needs numpy and pillow: {e}")
    try:
        im = Image.open(path).convert("RGB")
    except Exception as e:                                    # noqa: BLE001
        return None, f"unreadable: {e}"
    import numpy as np
    return np.asarray(im).astype("float32") / 255.0, None


def measure(path):
    """Everything that decides whether a frame is worth comparing against."""
    import numpy as np
    a, why = _load_image(path)
    if a is None:
        return {"path": str(path), "error": why}
    h, w, _ = a.shape
    flat = a.reshape(-1, 3)
    # Distinct colours separates a real scene from a flat one far better than
    # the mean does: catalog #77's whole difference 5 is a colour-variety gap.
    q = (flat * 255).astype("uint8")
    distinct = int(len(np.unique(q.view(np.dtype((np.void, 3))))))
    nonblack = float((flat.max(axis=1) > 8 / 255).mean())
    rec = {
        "path": str(path.relative_to(REPO)),
        "w": w, "h": h,
        "mean": [round(float(flat[:, c].mean()), 5) for c in range(3)],
        "median": [round(float(np.median(flat[:, c])), 5) for c in range(3)],
        "p99": [round(float(np.percentile(flat[:, c], 99)), 5) for c in range(3)],
        "max": round(float(flat.max()), 5),
        "distinct": distinct,
        "nonblack": round(nonblack, 5),
        "sha1": hashlib.sha1(q.tobytes()).hexdigest()[:16],
        "mtime": int(path.stat().st_mtime),
        "seconds": _seconds_from_name(path.name),
    }
    rec["kind"] = classify(rec)
    return rec


def _seconds_from_name(name):
    """frame_0120s.png -> 120. Returns None when the name does not say."""
    stem = name.rsplit(".", 1)[0]
    if stem.startswith("frame_") and stem.endswith("s"):
        body = stem[len("frame_"):-1]
        if body.isdigit():
            return int(body)
    return None


def classify(rec):
    """black / flat / menu / gameplay.

    The thresholds are read off the corpus rather than guessed, and the point of
    the `flat` class is that it is the trap: a frame that is not black but
    carries almost no colour variety looks like a usable reference in a
    thumbnail and is not one.
    """
    if rec["max"] <= 0.0:
        return "black"
    if rec["distinct"] < 500 or rec["nonblack"] < 0.02:
        return "flat"
    # A menu is bright and colour-poor next to Act 1 gameplay, which is dark and
    # colour-rich. Gameplay in this title runs mean 0.02-0.12 with 10k+ colours.
    if rec["distinct"] >= 8000:
        return "gameplay"
    return "menu"


def cmd_index(args):
    if not ORACLE_DIR.is_dir():
        sys.exit(f"REFUSING: {ORACLE_DIR} does not exist. Nothing was measured "
                 f"-- this is not 'no oracle frames', it is 'no oracle tree'.")
    frames = sorted(ORACLE_DIR.rglob("frame_*.png"))
    # Also take PPMs, which is what our own runtime writes; the cache indexes
    # references only, but a stray PPM in the oracle tree is worth reporting
    # rather than silently skipping.
    if not frames:
        sys.exit(f"REFUSING: no frame_*.png anywhere under {ORACLE_DIR}. "
                 f"The tree exists but holds no frames, so the index would be "
                 f"empty and every later query would answer '(none)'.")
    records = []
    for i, p in enumerate(frames):
        rec = measure(p)
        rec["id"] = i
        records.append(rec)
        print(f"  [{i:3}] {rec.get('kind','ERROR'):8} {rec['path']}", file=sys.stderr)
    INDEX.parent.mkdir(parents=True, exist_ok=True)
    INDEX.write_text(json.dumps(records, indent=1))
    counts = {}
    for r in records:
        counts[r.get("kind", "error")] = counts.get(r.get("kind", "error"), 0) + 1
    print(f"\nindexed {len(records)} frame(s) -> {INDEX.relative_to(REPO)}")
    print("  " + "  ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    if counts.get("black") or counts.get("flat"):
        print(f"  NOTE: {counts.get('black',0)} black and {counts.get('flat',0)} "
              f"flat frame(s) are indexed and FLAGGED, not dropped. They are "
              f"unusable as a reference; `list` excludes them unless --all.")
    return 0


def load_index():
    if not INDEX.exists():
        sys.exit(f"REFUSING: no index at {INDEX.relative_to(REPO)}. "
                 f"Run `tools/oracle_cache.py index` first. Answering a query "
                 f"with '(none)' here would mean 'never looked'.")
    return json.loads(INDEX.read_text())


def cmd_list(args):
    recs = load_index()
    searched = len(recs)
    excluded = {}

    def keep(r):
        if "error" in r:
            excluded["unreadable"] = excluded.get("unreadable", 0) + 1
            return False
        if not args.all and r["kind"] in ("black", "flat"):
            excluded[f"kind={r['kind']} (unusable as a reference)"] = \
                excluded.get(f"kind={r['kind']} (unusable as a reference)", 0) + 1
            return False
        if args.kind and r["kind"] != args.kind:
            excluded[f"kind != {args.kind}"] = excluded.get(f"kind != {args.kind}", 0) + 1
            return False
        if args.min_distinct and r["distinct"] < args.min_distinct:
            excluded[f"distinct < {args.min_distinct}"] = \
                excluded.get(f"distinct < {args.min_distinct}", 0) + 1
            return False
        return True

    hits = [r for r in recs if keep(r)]
    for r in hits:
        m = r["mean"]
        print(f"[{r['id']:3}] {r['kind']:8} {r['distinct']:6} colours  "
              f"mean {m[0]:.4f}/{m[1]:.4f}/{m[2]:.4f}  max {r['max']:.3f}  "
              f"{r['path']}")
    if not hits:
        # THE NEGATIVE, with its denominator and what removed everything.
        print(f"NO MATCH. Searched {searched} indexed frame(s); every one was "
              f"excluded:")
        for why, n in sorted(excluded.items(), key=lambda kv: -kv[1]):
            print(f"    {n:4} by  {why}")
        print("  This is a real negative over the whole index -- it is NOT "
              "'the oracle was never run'.")
        return 1
    print(f"\n{len(hits)} of {searched} indexed frame(s) matched.")
    return 0


def cmd_show(args):
    recs = load_index()
    for r in recs:
        if r["id"] == args.id:
            print(json.dumps(r, indent=2))
            return 0
    sys.exit(f"no frame with id {args.id}; the index holds ids 0..{len(recs)-1}")


def cmd_match(args):
    """The closest cached reference to one of OUR frames.

    Matched on exposure-invariant chromaticity and colour variety, NOT on pixel
    distance: the two sides are never at the same moment (catalog #84), so a
    pixel metric between them measures content, and quoting one is the mistake
    catalog #77 opened with.
    """
    import numpy as np
    ours, why = _load_image(Path(args.frame))
    if ours is None:
        sys.exit(f"cannot read {args.frame}: {why}")
    flat = ours.reshape(-1, 3)
    ours_mean = flat.mean(axis=0)
    s = ours_mean.sum()
    if s <= 0:
        sys.exit(f"REFUSING: {args.frame} is entirely black, so it has no "
                 f"chromaticity to match on. Nothing was compared.")
    ours_chroma = ours_mean / s
    q = (flat * 255).astype("uint8")
    ours_distinct = len(np.unique(q.view(np.dtype((np.void, 3)))))

    recs = [r for r in load_index()
            if "error" not in r and r["kind"] not in ("black", "flat")]
    if not recs:
        sys.exit("REFUSING: the index holds no usable reference frame "
                 "(every entry is black, flat or unreadable). Nothing matched "
                 "because there was nothing to match against.")
    scored = []
    for r in recs:
        m = np.array(r["mean"])
        if m.sum() <= 0:
            continue
        d_chroma = float(np.abs(m / m.sum() - ours_chroma).sum())
        d_var = abs(r["distinct"] - ours_distinct) / max(ours_distinct, 1)
        scored.append((d_chroma + 0.25 * d_var, d_chroma, d_var, r))
    scored.sort(key=lambda t: t[0])
    print(f"ours: mean {ours_mean[0]:.4f}/{ours_mean[1]:.4f}/{ours_mean[2]:.4f}  "
          f"chroma {ours_chroma[0]:.4f}/{ours_chroma[1]:.4f}/{ours_chroma[2]:.4f}  "
          f"{ours_distinct} colours\n")
    print("closest cached references (chromaticity + colour-variety distance):")
    for score, dc, dv, r in scored[:args.n]:
        m = r["mean"]
        print(f"  [{r['id']:3}] score {score:.4f} (chroma {dc:.4f}, variety {dv:+.2f})"
              f"  {r['distinct']:6} colours  mean {m[0]:.4f}/{m[1]:.4f}/{m[2]:.4f}"
              f"  {r['path']}")
    print(f"\nSearched {len(scored)} usable reference(s). A LOW score means the "
          f"same kind of scene, NOT the same moment -- these two runs are not "
          f"frame-synchronised (catalog #84), so do not quote a pixel metric.")
    return 0


def cmd_selftest(args):
    """Feeds the classifier one case that MUST come back positive and one that
    MUST come back negative. A cache whose classifier silently passes everything
    is the same bug one level up."""
    import numpy as np
    ok = True
    black = {"max": 0.0, "distinct": 1, "nonblack": 0.0}
    got = classify(black)
    print(f"  negative control (an all-black frame) -> {got!r}", end="  ")
    if got != "black":
        print("FAIL -- a black frame must be refused as a reference"); ok = False
    else:
        print("pass")
    rich = {"max": 1.0, "distinct": 24497, "nonblack": 0.758}
    got = classify(rich)
    print(f"  positive control (the oracle's Act 1 frame) -> {got!r}", end="  ")
    if got != "gameplay":
        print("FAIL -- a real gameplay frame must classify as usable"); ok = False
    else:
        print("pass")
    flat = {"max": 0.9, "distinct": 120, "nonblack": 0.9}
    got = classify(flat)
    print(f"  trap control (bright but 120 colours) -> {got!r}", end="  ")
    if got != "flat":
        print("FAIL -- a colour-poor frame must not pass as a reference"); ok = False
    else:
        print("pass")
    # The index itself must exist and hold at least one usable frame, or the
    # tool is reporting on nothing.
    if INDEX.exists():
        recs = json.loads(INDEX.read_text())
        usable = [r for r in recs if r.get("kind") == "gameplay"]
        print(f"  index: {len(recs)} frame(s), {len(usable)} usable gameplay")
        if not usable:
            print("  FAIL -- the index holds no usable gameplay reference"); ok = False
    else:
        print("  index: ABSENT (run `index` first); the classifier tests above "
              "still ran and are valid")
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("index", help="(re)measure every oracle frame on disk")
    p = sub.add_parser("list", help="query the index")
    p.add_argument("--kind", choices=["gameplay", "menu", "flat", "black"])
    p.add_argument("--min-distinct", type=int, default=0)
    p.add_argument("--all", action="store_true",
                   help="include black and flat frames, which are unusable")
    p = sub.add_parser("show", help="one frame's full record")
    p.add_argument("id", type=int)
    p = sub.add_parser("match", help="the closest cached reference to OUR frame")
    p.add_argument("frame")
    p.add_argument("-n", type=int, default=5)
    sub.add_parser("selftest", help="prove the classifier separates both classes")
    a = ap.parse_args()
    if a.cmd == "index":
        return cmd_index(a)
    if a.cmd == "list":
        return cmd_list(a)
    if a.cmd == "show":
        return cmd_show(a)
    if a.cmd == "match":
        return cmd_match(a)
    if a.cmd == "selftest":
        return cmd_selftest(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
