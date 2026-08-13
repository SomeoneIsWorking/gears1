#!/usr/bin/env python3
"""Refuse a native camera-pair frame carrying Gears' storage modal."""

from __future__ import annotations

import argparse
import csv
import io
from pathlib import Path

UI_VS = "5363d0746b3ef666"
UI_PS = "501ac5d8692bf7b6"


def count_pair(stream: io.TextIOBase) -> tuple[int, int, int]:
    rows = matches = run = max_run = 0
    for row in csv.DictReader(stream, delimiter="\t"):
        rows += 1
        matched = (row.get("vs_hash", "").lower() == UI_VS and
                   row.get("ps_hash", "").lower() == UI_PS)
        if matched:
            matches += 1
            run += 1
            max_run = max(max_run, run)
        else:
            run = 0
    return rows, matches, max_run


def classify(rows: int, matches: int, max_run: int) -> tuple[bool, str]:
    if rows == 0:
        return False, "no data rows"
    if max_run >= 8:
        return False, "the storage modal's measured eight-draw consecutive suffix is present"
    if max_run > 1:
        return False, "unknown consecutive UI-shader run"
    return True, "no storage-modal suffix"


def check(path: Path) -> int:
    if not path.is_file():
        print(f"REFUSING: UI-state draw table {path} is missing; scanned 0 rows, matched 0")
        return 2
    with path.open(newline="") as stream:
        rows, matches, max_run = count_pair(stream)
    accepted, kind = classify(rows, matches, max_run)
    if not accepted:
        print(f"REFUSING: scanned {rows} draw rows and matched {matches} occurrences of "
              f"{UI_VS}/{UI_PS}, longest consecutive run {max_run}: {kind}.")
        return 1
    print(f"UI STATE PASSES: scanned {rows} draw rows and matched {matches} occurrences "
          f"of {UI_VS}/{UI_PS}, longest consecutive run {max_run} ({kind}).")
    return 0


def selftest() -> int:
    header = "draw\tvs_hash\tps_hash\n"

    def fixture(runs: tuple[int, ...]) -> io.StringIO:
        lines = [header, "0\tdeadbeef\tfeedface\n"]
        draw = 1
        for n in runs:
            lines += [f"{draw + i}\t{UI_VS}\t{UI_PS}\n" for i in range(n)]
            draw += n
            lines.append(f"{draw}\tdeadbeef\tfeedface\n")
            draw += 1
        return io.StringIO("".join(lines))

    counts = [count_pair(fixture(runs)) for runs in
              ((1,), (1, 1), (1, 8), (8,), (), (3,), (1,) * 9)]
    got = [classify(*value)[0] for value in counts]
    want = [True, True, False, False, True, False, True]
    if got != want:
        print(f"FAIL: clean1/clean2/modal1+8/modal8/zero/unknown3/isolated9 "
              f"classes {got}, expected {want}")
        return 1
    print("PASS: UI-state discriminator accepts clean isolated/zero classes, "
          "refuses measured modal run=8 and unknown run=3")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("draws", nargs="?", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if args.draws is None:
        parser.error("draws.tsv is required unless --selftest is used")
    return check(args.draws)


if __name__ == "__main__":
    raise SystemExit(main())
