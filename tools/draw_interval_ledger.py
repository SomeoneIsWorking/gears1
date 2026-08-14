#!/usr/bin/env python3
"""Align one native/oracle draw interval and audit EDRAM ownership.

The interval bounds are half-open draw ordinals, normally the draws after one
resolve and through the draw preceding the next resolve.  The oracle order log
is produced by GEARS_ORACLE_DRAW_ORDER.  New logs append EDRAM mode,
RB_COLOR_INFO, RB_DEPTH_INFO and the normalized colour mask; an old log is
refused because shader alignment without resource ownership cannot answer which
pass produced a resolve.

Examples:
  tools/draw_interval_ledger.py --ours draws.tsv --oracle theirs_order.tsv \
      --ours-range 1609:1665 --oracle-range 1610:1669
  tools/draw_interval_ledger.py --selftest
"""

import argparse
import csv
from difflib import SequenceMatcher


def number(text):
    return int(text, 0)


def draw_range(text):
    try:
        lo, hi = (int(value, 0) for value in text.split(":", 1))
    except (ValueError, TypeError):
        raise argparse.ArgumentTypeError("expected START:END draw ordinals")
    if lo < 0 or hi <= lo:
        raise argparse.ArgumentTypeError("range must be non-empty and increasing")
    return lo, hi


def hex_field(text, width):
    return text.lower().removeprefix("0x").zfill(width)


def native_identity(row):
    return (
        hex_field(row["vs_hash"], 16), hex_field(row["ps_hash"], 16),
        hex_field(row["depth_control"], 8),
        hex_field(row["stencil_ref_mask_raw"], 8),
        hex_field(row["blend0"], 8), row["count"],
    )


def oracle_identity(row):
    return tuple(row[1:7])


def native_ownership(row):
    mode = number(row["edram_mode"])
    color = number(row["surface"])
    color_fmt = number(row["color_fmt"])
    depth = number(row["depth_base"])
    mask = number(row["color_mask"])
    return mode, color, color_fmt, depth, mask


NATIVE_REQUIRED_FIELDS = (
    "vs_hash", "ps_hash", "depth_control", "stencil_ref_mask_raw",
    "blend0", "count", "edram_mode", "surface", "color_fmt",
    "depth_base", "color_mask",
)


def is_native_draw(row):
    """True only for raster draw rows, never resolve/copy diagnostic rows."""
    return row.get("draw", "").isdigit() and all(
        row.get(field, "") != "" for field in NATIVE_REQUIRED_FIELDS)


def oracle_ownership(row):
    mode, color_info, depth_info, mask = (
        number(row[11]), int(row[12], 16), int(row[13], 16), int(row[14], 16))
    return mode, color_info & 0xFFF, (color_info >> 16) & 0xF, depth_info & 0xFFF, mask


def ownership_text(value):
    mode, color, fmt, depth, mask = value
    return f"mode={mode} C{color:03X}/f{fmt} write={mask:X} D{depth:03X}"


def align(native, oracle):
    return SequenceMatcher(
        None, [native_identity(row) for row in native],
        [oracle_identity(row) for row in oracle], autojunk=False,
    ).get_opcodes()


def audit(native, oracle):
    opcodes = align(native, oracle)
    exact = sum(i2 - i1 for tag, i1, i2, _, _ in opcodes if tag == "equal")
    ownership_mismatches = []
    for tag, i1, i2, j1, _ in opcodes:
        if tag != "equal":
            continue
        for offset in range(i2 - i1):
            nrow, orow = native[i1 + offset], oracle[j1 + offset]
            no, oo = native_ownership(nrow), oracle_ownership(orow)
            if no != oo:
                ownership_mismatches.append((nrow, orow, no, oo))
    return opcodes, exact, ownership_mismatches


def selftest():
    base = {
        "vs_hash": "1", "ps_hash": "2", "depth_control": "0x3",
        "stencil_ref_mask_raw": "0x4", "blend0": "0x5", "count": "6",
        "edram_mode": "4", "surface": "0x2d0", "color_fmt": "12",
        "depth_base": "0x0", "color_mask": "15", "draw": "10",
    }
    oracle = ["10", "0000000000000001", "0000000000000002", "00000003",
              "00000004", "00000005", "6", "0", "0", "1280", "720",
              "4", "000c02d0", "00000000", "f"]
    _, exact, mismatches = audit([base], [oracle])
    assert exact == 1 and not mismatches

    wrong = oracle.copy()
    wrong[12] = "000c0400"
    _, exact, mismatches = audit([base], [wrong])
    assert exact == 1 and len(mismatches) == 1

    inserted = oracle.copy()
    inserted[0] = "11"
    inserted[6] = "9"
    opcodes, exact, _ = audit([base], [oracle, inserted])
    assert exact == 1 and any(tag == "insert" for tag, *_ in opcodes)

    resolve = base.copy()
    resolve["depth_base"] = ""
    assert is_native_draw(base) and not is_native_draw(resolve)
    print("draw interval ledger selftest: positive match, ownership mismatch, "
          "inserted-draw negative and resolve-row rejection all fired")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--ours")
    parser.add_argument("--oracle")
    parser.add_argument("--ours-range", type=draw_range)
    parser.add_argument("--oracle-range", type=draw_range)
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return 0
    if not all((args.ours, args.oracle, args.ours_range, args.oracle_range)):
        parser.error("--ours, --oracle, --ours-range and --oracle-range are required")

    with open(args.ours, newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    native_candidates = [row for row in rows if row["draw"].isdigit() and
              args.ours_range[0] <= int(row["draw"]) < args.ours_range[1]]
    native = [row for row in native_candidates if is_native_draw(row)]
    skipped_native = len(native_candidates) - len(native)
    with open(args.oracle) as stream:
        all_oracle = [line.rstrip("\n").split("\t") for line in stream if line.strip()]
    candidates = [row for row in all_oracle if len(row) >= 1 and
                  args.oracle_range[0] <= int(row[0]) < args.oracle_range[1]]
    oracle = [row for row in candidates if len(row) >= 15]
    if len(oracle) != len(candidates):
        print(f"REFUSING: scanned {len(candidates)} oracle draw(s), but only "
              f"{len(oracle)} carry the four ownership columns. Rebuild the "
              "oracle and recapture; a shader-only ledger cannot see the "
              "resource graph.")
        return 2
    if not native or not oracle:
        print(f"REFUSING: scanned {len(rows)} native and {len(all_oracle)} oracle "
              f"rows; selected {len(native)} and {len(oracle)}. The requested "
              "interval is absent, not equal.")
        return 2

    opcodes, exact, mismatches = audit(native, oracle)
    print(f"scanned {len(native)} native and {len(oracle)} oracle draws; "
          f"{exact} exact shader/state/geometry matches; skipped "
          f"{skipped_native} native resolve/copy row(s)")
    for tag, i1, i2, j1, j2 in opcodes:
        if tag == "equal":
            continue
        nr = f"{native[i1]['draw']}..{native[i2 - 1]['draw']}" if i1 < i2 else "--"
        ora = f"{oracle[j1][0]}..{oracle[j2 - 1][0]}" if j1 < j2 else "--"
        print(f"{tag.upper():7} native {nr:>12} oracle {ora:>12}")
        for row in native[i1:i2]:
            print(f"  N {row['draw']:>5} {native_identity(row)} "
                  f"{ownership_text(native_ownership(row))}")
        for row in oracle[j1:j2]:
            print(f"  O {row[0]:>5} {oracle_identity(row)} "
                  f"{ownership_text(oracle_ownership(row))}")
    if mismatches:
        print(f"OWNERSHIP MISMATCHES: {len(mismatches)} among {exact} aligned draws")
        for nrow, orow, no, oo in mismatches:
            print(f"  native {nrow['draw']} {ownership_text(no)} != oracle "
                  f"{orow[0]} {ownership_text(oo)}")
    else:
        print(f"OWNERSHIP MATCH: all {exact} aligned draws use the same EDRAM "
              "mode, colour base/format/write mask and depth base")
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
