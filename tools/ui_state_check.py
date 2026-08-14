#!/usr/bin/env python3
"""Compare the consecutive UI-shader run shape of a camera-pair frame."""

from __future__ import annotations

import argparse
import csv
import io
from pathlib import Path

UI_VS = "5363d0746b3ef666"
UI_PS = "501ac5d8692bf7b6"


def count_pair(stream: io.TextIOBase, *, oracle: bool = False) -> tuple[int, int, int]:
    rows = matches = run = max_run = 0
    if oracle:
        parsed = (line.rstrip("\n").split("\t") for line in stream if line.strip())
        records = ((fields[1] if len(fields) > 1 else "",
                    fields[2] if len(fields) > 2 else "") for fields in parsed)
    else:
        records = ((row.get("vs_hash", ""), row.get("ps_hash", ""))
                   for row in csv.DictReader(stream, delimiter="\t"))
    for vs_hash, ps_hash in records:
        rows += 1
        matched = (vs_hash.lower() == UI_VS and ps_hash.lower() == UI_PS)
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


def compare(native_path: Path, oracle_path: Path) -> int:
    missing = [str(p) for p in (native_path, oracle_path) if not p.is_file()]
    if missing:
        print(f"REFUSING: missing UI-state draw table(s): {', '.join(missing)}; "
              "the two states were not compared.")
        return 2
    with native_path.open(newline="") as stream:
        native = count_pair(stream)
    with oracle_path.open(newline="") as stream:
        oracle = count_pair(stream, oracle=True)
    if native[0] == 0 or oracle[0] == 0:
        print(f"REFUSING: UI-state comparison scanned native/oracle rows "
              f"{native[0]}/{oracle[0]}; an empty side proves no equality.")
        return 2
    if native[2] != oracle[2]:
        print(f"REFUSING: UI-state run shape differs: native scanned {native[0]} rows, "
              f"matched {native[1]}, longest run {native[2]}; oracle scanned "
              f"{oracle[0]}, matched {oracle[1]}, longest run {oracle[2]}.")
        return 1
    print(f"UI STATES MATCH: native/oracle longest consecutive run {native[2]} "
          f"(rows {native[0]}/{oracle[0]}, matches {native[1]}/{oracle[1]}).")
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
    def oracle_fixture(runs: tuple[int, ...]) -> io.StringIO:
        lines, draw = [], 0
        for n in runs:
            lines += [f"{draw + i}\t{UI_VS}\t{UI_PS}\n" for i in range(n)]
            draw += n
            lines.append(f"{draw}\tdeadbeef\tfeedface\n")
            draw += 1
        return io.StringIO("".join(lines))

    paired = [
        (count_pair(fixture((1,))), count_pair(oracle_fixture((1,)), oracle=True), True),
        (count_pair(fixture((1, 8))), count_pair(oracle_fixture((1,) * 8), oracle=True), False),
        (count_pair(fixture((1, 12))), count_pair(oracle_fixture((1, 12)), oracle=True), True),
    ]
    pair_got = [native[2] == oracle[2] for native, oracle, _ in paired]
    pair_want = [want for _, _, want in paired]
    if pair_got != pair_want:
        print(f"FAIL: paired clean/modal-mismatch/shared-run classes {pair_got}, "
              f"expected {pair_want}")
        return 1
    print("PASS: paired discriminator accepts matching run shapes 1 and 12, and "
          "refuses native modal run 8 against oracle isolated runs")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("draws", nargs="?", type=Path)
    parser.add_argument("--oracle", type=Path,
                        help="oracle's headerless draw-order table; require equal run shape")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if args.draws is None:
        parser.error("draws.tsv is required unless --selftest is used")
    return compare(args.draws, args.oracle) if args.oracle else check(args.draws)


if __name__ == "__main__":
    raise SystemExit(main())
