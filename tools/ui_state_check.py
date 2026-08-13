#!/usr/bin/env python3
"""Refuse a native camera-pair frame carrying Gears' storage modal."""

from __future__ import annotations

import argparse
import csv
import io
from pathlib import Path

UI_VS = "5363d0746b3ef666"
UI_PS = "501ac5d8692bf7b6"


def count_pair(stream: io.TextIOBase) -> tuple[int, int]:
    rows = matches = 0
    for row in csv.DictReader(stream, delimiter="\t"):
        rows += 1
        if row.get("vs_hash", "").lower() == UI_VS and row.get("ps_hash", "").lower() == UI_PS:
            matches += 1
    return rows, matches


def classify(rows: int, matches: int) -> tuple[bool, str]:
    if rows == 0:
        return False, "no data rows"
    if matches == 0:
        return False, "the reference UI draw is absent, so the discriminator is blind"
    if matches in (1, 2):
        return True, "measured clean gameplay class"
    if matches in (9, 10):
        return False, "measured clean class plus the storage modal's eight-draw suffix"
    return False, "unknown UI state"


def check(path: Path) -> int:
    if not path.is_file():
        print(f"REFUSING: UI-state draw table {path} is missing; scanned 0 rows, matched 0")
        return 2
    with path.open(newline="") as stream:
        rows, matches = count_pair(stream)
    accepted, kind = classify(rows, matches)
    if not accepted:
        print(f"REFUSING: scanned {rows} draw rows and matched {matches} occurrences of "
              f"{UI_VS}/{UI_PS}: {kind}.")
        return 1
    print(f"UI STATE PASSES: scanned {rows} draw rows and matched {matches} occurrences "
          f"of {UI_VS}/{UI_PS} ({kind}); the eight-draw storage modal suffix is absent.")
    return 0


def selftest() -> int:
    header = "draw\tvs_hash\tps_hash\n"

    def fixture(n: int) -> io.StringIO:
        lines = [header, "0\tdeadbeef\tfeedface\n"]
        lines += [f"{i + 1}\t{UI_VS}\t{UI_PS}\n" for i in range(n)]
        return io.StringIO("".join(lines))

    counts = [count_pair(fixture(n)) for n in (1, 2, 9, 10, 0, 3)]
    got = [classify(*value)[0] for value in counts]
    want = [True, True, False, False, False, False]
    if got != want:
        print(f"FAIL: clean1/clean2/modal9/modal10/blind/unknown classes {got}, expected {want}")
        return 1
    print("PASS: UI-state discriminator accepts clean=1/2 and refuses modal=9/10, blind=0, unknown=3")
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
