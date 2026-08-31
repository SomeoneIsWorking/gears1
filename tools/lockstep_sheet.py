#!/usr/bin/env python3
"""Put our frame N beside the oracle's frame N, for every N both sides wrote.

    tools/lockstep_sheet.py [--dir scratch/oracle/lockstep] [--out sheet.png]
                            [--gamma 0.3] [--from N] [--to N]

Reads the filmstrips `tools/oracle_lockstep.py` leaves under `<dir>/ours` and
`<dir>/theirs` and writes one contact sheet with the two sides as adjacent
columns, one row per guest frame index they share.

JOINED ON THE GUEST'S PRESENT COUNTER, which is what both filenames now mean --
ours `frame_%05d.ppm`, the oracle's `frame_%06d.png`. That was not always true:
our side used to be numbered by frames THIS RENDERER DREW, and since the backend
returns early from a present it has nothing to render, a run measured 1,500
rendered against 12,540 presented. Joining those two filmstrips by number put
moments eight times apart side by side and called them the same frame.

GAMMA-BOOSTED BY DEFAULT, and that is not cosmetic. Act 1 is a dark scene: a
straight look at these frames shows a bright doorway on black, and the black is
fully rendered geometry sitting between 0.002 and 0.02. Reading it as "not
rendered" is how catalog #86 came to be filed and withdrawn. `--gamma 1` gives
the untouched image; the sheet always says which was used.

WHAT A ROW DOES NOT PROVE. Frame N is the same game moment on both sides only as
far as the title is deterministic under identical input, which is a property of
the TITLE and is not established -- read `<dir>/manifest.txt` and its ours/ours2
control arm first. This tool refuses to run if the manifest is missing, because
a sheet with no provenance is exactly the artefact that gets quoted later.
"""
import argparse
import re
import sys
from pathlib import Path

OURS_RE = re.compile(r"frame_(\d+)\.ppm$")
THEIRS_RE = re.compile(r"frame_(\d+)\.(png|ppm)$")


def index_of(path, pattern):
    m = pattern.search(path.name)
    return int(m.group(1)) if m else None


def collect(directory, pattern):
    """{guest frame index: path}. Empty when the directory has no filmstrip."""
    if not directory.is_dir():
        return {}
    out = {}
    for p in sorted(directory.iterdir()):
        n = index_of(p, pattern)
        if n is not None:
            out[n] = p
    return out


def main(argv):
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default="scratch/oracle/lockstep")
    ap.add_argument("--ours", default="ours",
                    help="which of our runs to use, 'ours' or 'ours2'. The two"
                         " are the determinism control arm and they do not"
                         " always agree on how far they got -- a measured"
                         " 9-of-25 against 25-of-25 on the same walk -- so"
                         " which one is being shown has to be a stated choice,"
                         " never whichever happens to be first")
    ap.add_argument("--out", default=None)
    ap.add_argument("--gamma", type=float, default=0.3,
                    help="exponent applied to luminance; 1 leaves the image alone")
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=1 << 30)
    ap.add_argument("--width", type=int, default=480, help="per-panel width")
    args = ap.parse_args(argv[1:])

    try:
        from PIL import Image
        import numpy as np
    except ImportError as exc:
        print(f"REFUSING: {exc}. Nothing was compared.")
        return 2

    root = Path(args.dir)
    manifest = root / "manifest.txt"
    if not manifest.is_file():
        # Not pedantry: the manifest carries the walk, which side ran short and
        # whether the oracle lost its GPU. A sheet built without it looks
        # identical to one built from a healthy pair.
        print(f"REFUSING: {manifest} does not exist, so this filmstrip has no"
              f" provenance -- no walk, no ours/ours2 control, no record of a"
              f" side that died. Run tools/oracle_lockstep.py. Nothing was"
              f" compared.")
        return 1

    ours = collect(root / args.ours, OURS_RE)
    theirs = collect(root / "theirs", THEIRS_RE)
    if not ours or not theirs:
        print(f"REFUSING: ours has {len(ours)} frame(s) and theirs has"
              f" {len(theirs)}. A side with none did not run; it did not agree."
              f" Nothing was compared.")
        return 1

    shared = sorted(n for n in set(ours) & set(theirs) if args.lo <= n <= args.hi)
    only_ours = sorted(set(ours) - set(theirs))
    only_theirs = sorted(set(theirs) - set(ours))

    print(f"{args.ours:<6} {len(ours)} frames, {min(ours)}..{max(ours)}")
    print(f"theirs {len(theirs)} frames, {min(theirs)}..{max(theirs)}")
    print(f"shared {len(shared)} frame index(es)"
          + (f" in [{args.lo},{args.hi}]" if args.hi < (1 << 30) or args.lo else ""))
    # ALWAYS printed, including when empty: a side that stopped early is the
    # single most likely reason a comparison is thin, and "(none)" has to be
    # distinguishable from "never looked".
    print(f"  only ours   ({len(only_ours)}): "
          + (", ".join(map(str, only_ours[:12])) + ("..." if len(only_ours) > 12
                                                    else "") or "(none)"))
    print(f"  only theirs ({len(only_theirs)}): "
          + (", ".join(map(str, only_theirs[:12])) + ("..." if len(only_theirs) > 12
                                                      else "") or "(none)"))
    if not shared:
        print("REFUSING: the two sides share NO frame index, so there is no row"
              " to draw. The filmstrips do not overlap -- check the manifest for"
              " a side that ran short. Nothing was written.")
        return 1

    def panel(path):
        im = Image.open(path).convert("RGB")
        a = np.asarray(im).astype(np.float32) / 255.0
        if args.gamma != 1.0:
            a = np.clip(a, 0.0, 1.0) ** args.gamma
        h = max(1, round(args.width * im.height / im.width))
        return Image.fromarray((a * 255).astype("uint8")).resize((args.width, h))

    first = panel(ours[shared[0]])
    pw, ph = first.size
    label = 16
    sheet = Image.new("RGB", (pw * 2, (ph + label) * len(shared)), (0, 0, 0))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(sheet)
    for row, n in enumerate(shared):
        y = row * (ph + label)
        draw.text((4, y + 3), f"guest frame {n}   ours (left)  |  oracle (right)"
                              f"   gamma {args.gamma}", fill=(200, 200, 200))
        sheet.paste(panel(ours[n]), (0, y + label))
        sheet.paste(panel(theirs[n]), (pw, y + label))

    out = Path(args.out) if args.out else root / f"sheet_gamma{args.gamma}.png"
    sheet.save(out)
    print(f"\nwrote {out} -- {len(shared)} row(s), ours left, oracle right,"
          f" gamma {args.gamma}")
    print("Frame N is the same game moment on the two sides only as far as the"
          " title is\ndeterministic under identical input. Read"
          f" {manifest} and its ours/ours2\ncontrol arm before calling any row a"
          " rendering difference.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
