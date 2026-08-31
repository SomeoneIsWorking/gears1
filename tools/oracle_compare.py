#!/usr/bin/env python3
"""Capture headless wall-clock filmstrips from native and oracle renderers."""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

from gearsue3_bootstrap.environment import EnvironmentError, load_environment
from gearsue3_bootstrap.paths import BuildPathError, build_directory
from gearsue3_bootstrap.process import terminate_child
from gearsue3_bootstrap.profile import load_profile, native_oracle_compare_schedule
from replay_corpus import REPO_ROOT, ReplayCorpusError, reset_scratch_directory


def _bounded_run(
    command: list[Path | str],
    environment: dict[str, str],
    log_path: Path,
    duration: int,
) -> int:
    with log_path.open("wb") as log:
        child = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            try:
                return child.wait(timeout=duration)
            except subprocess.TimeoutExpired:
                terminate_child(child)
                return 0
        finally:
            terminate_child(child)


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if len(argv) > 2:
        print("usage: tools/oracle_compare.py [seconds] [interval]", file=sys.stderr)
        return 2
    try:
        seconds = int(argv[0]) if argv else 240
        interval = int(argv[1]) if len(argv) == 2 else 30
    except ValueError:
        print("oracle_compare: seconds and interval must be integers", file=sys.stderr)
        return 2
    if seconds <= 0 or interval <= 0:
        print("oracle_compare: seconds and interval must be positive", file=sys.stderr)
        return 2
    try:
        environment = load_environment(REPO_ROOT)
        build = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
        output = reset_scratch_directory(REPO_ROOT / "scratch/oracle/compare")
    except (BuildPathError, EnvironmentError, ReplayCorpusError) as error:
        print(f"oracle_compare: REFUSING: {error}", file=sys.stderr)
        return 2
    game = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
    runtime = build / "runtime/gears1"
    oracle = REPO_ROOT / "build/oracle/xenia_oracle"
    executable = game / "default.xex"
    for required in (runtime, oracle, executable):
        if not required.is_file():
            print(f"oracle_compare: REFUSING: missing {required}", file=sys.stderr)
            return 2
    image = environment.get("GEARS_ISO")
    if image and Path(image).is_file():
        oracle_target = Path(image)
        oracle_source = f"the disc image ({oracle_target})"
    else:
        oracle_target = executable
        oracle_source = f"the extracted tree ({executable}); GEARS_ISO unset"

    navigation = load_profile(REPO_ROOT).navigation
    native_input = native_oracle_compare_schedule(navigation, seconds)
    native_environment = {
        **environment,
        "GEARS_NO_WINDOW": "1",
        "GEARS_INPUT_SCRIPT": native_input,
        "GEARS_DRAW_FRAME_AT": "1",
        "GEARS_DRAW_FRAME_COUNT": "0",
        "GEARS_DRAW_FRAME_REPORT_EVERY": str(interval * 30),
        "GEARS_DRAW_DIR": str(output / "ours"),
    }
    (output / "ours").mkdir()
    (output / "theirs").mkdir()
    print(f"== native renderer, headless, {seconds}s ==")
    _bounded_run(
        [runtime, executable, game], native_environment, output / "ours.log", seconds
    )
    print(f"== oracle renderer, headless, {seconds}s ==")
    oracle_environment = {**environment, "SDL_AUDIODRIVER": "dummy"}
    _bounded_run(
        [
            oracle,
            "--store_shaders=false",
            f"--target={oracle_target}",
            f"--oracle_out={output / 'theirs'}",
            f"--oracle_seconds={seconds}",
            f"--oracle_interval={interval}",
            f"--oracle_input={navigation.oracle_compare_input}",
        ],
        oracle_environment,
        output / "theirs.log",
        seconds + 20,
    )
    ours_count = len(tuple((output / "ours").glob("*.ppm")))
    theirs_count = len(tuple((output / "theirs").glob("*.png")))
    crashes = (output / "theirs.log").read_text(errors="replace").count("CRASH DUMP")
    manifest = (
        f"native walk: {native_input}\n"
        f"oracle walk: {navigation.oracle_compare_input}\n"
        f"oracle booted from: {oracle_source}\n"
        f"ours: {ours_count} frames\n"
        f"theirs: {theirs_count} frames\n"
        f"oracle guest crashes: {crashes}\n\n"
        "These are separate emulations at matching wall-clock offsets, not "
        "frame-synchronised. Do not compute a pixel metric between them.\n"
    )
    print(manifest, end="")
    (output / "manifest.txt").write_text(manifest, encoding="utf-8")
    if ours_count == 0 or theirs_count == 0:
        print("FAILED: one side produced no frames; there is nothing to compare.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
