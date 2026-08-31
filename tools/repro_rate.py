#!/usr/bin/env python3
"""Measure crash/stall outcomes across isolated, bounded headless runs."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from gearsue3_bootstrap.environment import EnvironmentError, load_environment
from gearsue3_bootstrap.paths import BuildPathError, build_directory
from gearsue3_bootstrap.process import terminate_child
from gearsue3_bootstrap.profile import load_profile
from replay_corpus import REPO_ROOT, ReplayCorpusError, reset_scratch_directory

_STALL = re.compile(r"[a-z_]+ has (?:made no progress for [0-9]+ s|NEVER made progress)")
_FRAMES = re.compile(r"VdSwap: ([0-9]+) frames")


@dataclass(frozen=True)
class RunResult:
    number: int
    timed_out: bool
    returncode: int
    log_path: Path


def classify(timed_out: bool, log: str) -> str:
    if timed_out and _STALL.search(log):
        return "STALLED"
    if timed_out:
        return "clean"
    return "CRASHED"


def _one_run(
    number: int,
    seconds: int,
    binary: Path,
    executable: Path,
    game: Path,
    output: Path,
    environment: dict[str, str],
    schedule: str,
) -> RunResult:
    home = output / f"home{number}"
    home.mkdir()
    log_path = output / f"run{number}.log"
    run_environment = {
        **environment,
        "XDG_DATA_HOME": str(home),
        "GEARS_INPUT_SCRIPT": schedule,
        "GEARS_NO_WINDOW": "1",
        "GEARS_AUDIO_OUT": "0",
    }
    timed_out = False
    with log_path.open("wb") as log:
        child = subprocess.Popen(
            [binary, executable, game],
            cwd=REPO_ROOT,
            env=run_environment,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            try:
                child.wait(timeout=seconds)
            except subprocess.TimeoutExpired:
                timed_out = True
                terminate_child(child, 20)
        finally:
            terminate_child(child, 20)
    return RunResult(number, timed_out, child.returncode or 0, log_path)


def _selftest() -> int:
    cases = (
        (True, "audio has made no progress for 9 s", "STALLED"),
        (True, "stall detector armed", "clean"),
        (True, "xma has NEVER made progress", "STALLED"),
        (False, "address: fault", "CRASHED"),
    )
    failures = 0
    for timed_out, log, expected in cases:
        observed = classify(timed_out, log)
        print(f"{'ok' if observed == expected else 'FAIL'}: expected {expected}, got {observed}")
        failures += observed != expected
    print(f"{len(cases) - failures} of {len(cases)} classifier cases pass")
    return 1 if failures else 0


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if argv == ["--selftest"]:
        return _selftest()
    if len(argv) > 3:
        print("usage: tools/repro_rate.py [runs] [seconds] [parallel]", file=sys.stderr)
        return 2
    try:
        runs = int(argv[0]) if argv else 8
        seconds = int(argv[1]) if len(argv) >= 2 else 170
        parallel = int(argv[2]) if len(argv) == 3 else 4
    except ValueError:
        print("repro_rate: runs, seconds, and parallel must be integers", file=sys.stderr)
        return 2
    if runs <= 0 or seconds <= 0 or parallel <= 0 or parallel > runs:
        print("repro_rate: require runs/seconds > 0 and 0 < parallel <= runs", file=sys.stderr)
        return 2
    try:
        environment = load_environment(REPO_ROOT)
        build = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
        output = reset_scratch_directory(REPO_ROOT / "scratch/logs/repro-rate")
    except (BuildPathError, EnvironmentError, ReplayCorpusError) as error:
        print(f"repro_rate: REFUSING: {error}", file=sys.stderr)
        return 2
    binary = Path(environment.get("GEARS_BINARY", build / "runtime/gears1"))
    game = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
    executable = game / "default.xex"
    for required in (binary, executable):
        if not required.is_file():
            print(f"repro_rate: REFUSING: missing {required}", file=sys.stderr)
            return 2
    navigation = load_profile(REPO_ROOT).navigation
    schedule = environment.get("GEARS_INPUT_SCRIPT", navigation.repro_rate_walk)
    with ThreadPoolExecutor(max_workers=parallel) as executor:
        futures = [
            executor.submit(
                _one_run,
                number,
                seconds,
                binary,
                executable,
                game,
                output,
                environment,
                schedule,
            )
            for number in range(1, runs + 1)
        ]
        results = [future.result() for future in futures]
    totals = {"CRASHED": 0, "STALLED": 0, "clean": 0}
    armed = 0
    for result in sorted(results, key=lambda value: value.number):
        log = result.log_path.read_text(errors="replace")
        outcome = classify(result.timed_out, log)
        totals[outcome] += 1
        armed += "stall detector armed" in log
        frames = _FRAMES.findall(log)
        frame_text = frames[-1] if frames else "?"
        detail = _STALL.search(log)
        print(
            f"run {result.number}: {outcome} exit={result.returncode} frames={frame_text}"
        )
        if detail:
            print(f"    {detail.group(0)}")
    print(
        f"\n{totals['CRASHED']} crashed, {totals['STALLED']} stalled, "
        f"{totals['clean']} clean, out of {runs} runs at {seconds}s each; "
        f"parallel={parallel}; detector armed in {armed} run(s)."
    )
    print(f"Logs: {output}")
    print("The time cap makes this a lower bound on failures, not a rate estimate.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
