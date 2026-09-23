#!/usr/bin/env python3
"""Name the translated guest functions in a perf recording of the product, per thread.

Xenia keeps translated code in a shared-memory file mapping, so `perf report`
attributes those samples to that file and never consults the product's
``/tmp/perf-<pid>.map``. This tool resolves every sample's instruction pointer
against the map itself:

    uv run --locked python tools/run_offscreen.py --walk gameplay --seconds 300 --perf-map
    perf record -F 999 -p <pid> -o scratch/perf/guest.data -- sleep 8
    uv run --locked python tools/perf_guest_report.py scratch/perf/guest.data /tmp/perf-<pid>.map

Each thread's line states its sample count; samples outside every translated
function are counted as host code. A recording in which no sample falls in a
translated function is refused rather than reported as all-host.
"""

from __future__ import annotations

import argparse
import bisect
import subprocess
import sys
from collections import Counter
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

HOST = "host"


@dataclass(frozen=True)
class Symbol:
    start: int
    size: int
    name: str


class PerfMap:
    """The translated functions in a perf map, looked up by host address."""

    def __init__(self, symbols: Iterable[Symbol]) -> None:
        self._symbols = sorted(symbols, key=lambda symbol: symbol.start)
        self._starts = [symbol.start for symbol in self._symbols]

    def __len__(self) -> int:
        return len(self._symbols)

    def name(self, address: int) -> str:
        index = bisect.bisect_right(self._starts, address) - 1
        if index >= 0:
            symbol = self._symbols[index]
            if address < symbol.start + symbol.size:
                return symbol.name
        return HOST


def parse_perf_map(lines: Iterable[str]) -> PerfMap:
    """Lines of ``<hex start> <hex size> <name>``; any other line is refused by number."""

    symbols = []
    for number, line in enumerate(lines, start=1):
        fields = line.split(maxsplit=2)
        if not fields:
            continue
        try:
            symbols.append(Symbol(int(fields[0], 16), int(fields[1], 16), fields[2].strip()))
        except (IndexError, ValueError) as error:
            raise ValueError(f"perf map line {number} is not '<start> <size> <name>': {line!r}") from error
    if not symbols:
        raise ValueError("the perf map names no translated function")
    return PerfMap(symbols)


def parse_samples(lines: Iterable[str]) -> list[tuple[str, int]]:
    """``perf script -F comm,ip`` output: a thread name, which may hold spaces, then an address."""

    samples = []
    for number, line in enumerate(lines, start=1):
        if not line.strip():
            continue
        thread, _, address = line.strip().rpartition(" ")
        try:
            samples.append((thread.strip(), int(address, 16)))
        except ValueError as error:
            raise ValueError(f"sample line {number} does not end in an address: {line!r}") from error
    if not samples:
        raise ValueError("the recording holds no samples")
    return samples


def attribute(samples: Iterable[tuple[str, int]], perf_map: PerfMap) -> dict[str, Counter[str]]:
    """Sample counts per thread and function; the host bucket collects the rest."""

    threads: dict[str, Counter[str]] = {}
    for thread, address in samples:
        threads.setdefault(thread, Counter())[perf_map.name(address)] += 1
    if all(set(functions) == {HOST} for functions in threads.values()):
        raise ValueError(
            f"no sample of {sum(sum(f.values()) for f in threads.values())} falls in any of the "
            f"map's {len(perf_map)} translated functions; the map belongs to another process "
            "or the run was not given --perf-map"
        )
    return threads


def render(threads: dict[str, Counter[str]], thread_count: int, function_count: int) -> str:
    total = sum(sum(functions.values()) for functions in threads.values())
    ranked = sorted(threads.items(), key=lambda item: -sum(item[1].values()))
    lines = []
    for thread, functions in ranked[:thread_count]:
        samples = sum(functions.values())
        lines.append(f"{thread}: {samples} samples, {100 * samples / total:.1f}% of {total}")
        for name, count in functions.most_common(function_count):
            lines.append(f"  {100 * count / samples:5.1f}%  {name}")
    return "\n".join(lines)


def _script_samples(data: Path) -> list[str]:
    result = subprocess.run(
        ["perf", "script", "-i", str(data), "-F", "comm,ip"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.splitlines()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("data", type=Path, help="a perf.data recording of the product")
    parser.add_argument("map", type=Path, help="the run's /tmp/perf-<pid>.map")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--functions", type=int, default=12)
    arguments = parser.parse_args(argv)
    try:
        perf_map = parse_perf_map(arguments.map.read_text().splitlines())
        threads = attribute(parse_samples(_script_samples(arguments.data)), perf_map)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"perf_guest_report: {error}", file=sys.stderr)
        return 1
    print(render(threads, arguments.threads, arguments.functions))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
