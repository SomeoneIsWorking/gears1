#!/usr/bin/env python3
"""Capture frame-indexed native/native-control/oracle filmstrips."""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

from gearsue3_bootstrap.environment import EnvironmentError, load_environment
from gearsue3_bootstrap.paths import BuildPathError, build_directory
from gearsue3_bootstrap.process import terminate_child, terminate_process_group
from gearsue3_bootstrap.profile import (
    ProfileError,
    last_frame,
    load_profile,
    native_schedule,
    oracle_schedule,
    parse_frame_walk,
)
from replay_corpus import REPO_ROOT, ReplayCorpusError, reset_scratch_directory


def _native_run(
    runtime: Path,
    executable: Path,
    game: Path,
    destination: Path,
    log_path: Path,
    environment: dict[str, str],
    schedule: str,
    frames: int,
    interval: int,
    timeout: int,
) -> None:
    run_environment = {
        **environment,
        "GEARS_NO_WINDOW": "1",
        "GEARS_INPUT_SCRIPT": schedule,
        "GEARS_DRAW_FRAME_AT": "1",
        "GEARS_DRAW_FRAME_COUNT": "0",
        "GEARS_DRAW_FRAME_REPORT_EVERY": str(interval),
        "GEARS_DRAW_DIR": str(destination),
    }
    expected = frames // interval
    with log_path.open("wb") as log:
        child = subprocess.Popen(
            [runtime, executable, game],
            cwd=REPO_ROOT,
            env=run_environment,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + timeout
        try:
            while child.poll() is None and time.monotonic() < deadline:
                if len(tuple(destination.glob("frame_*.ppm"))) >= expected:
                    break
                time.sleep(0.25)
        finally:
            terminate_child(child, 20)


def _oracle_run(
    oracle: Path,
    target: Path,
    destination: Path,
    log_path: Path,
    environment: dict[str, str],
    schedule: str,
    frames: int,
    interval: int,
    timeout: int,
) -> None:
    command = [
        oracle,
        "--store_shaders=false",
        f"--target={target}",
        f"--oracle_out={destination}",
        "--oracle_by_frame=true",
        f"--oracle_frames={frames}",
        f"--oracle_frame_interval={interval}",
        f"--oracle_input={schedule}",
    ]
    with log_path.open("wb") as log:
        child = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            env={**environment, "SDL_AUDIODRIVER": "dummy"},
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            try:
                child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                terminate_process_group(child, 20)
        finally:
            terminate_process_group(child, 20)


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if len(argv) > 2:
        print("usage: tools/oracle_lockstep.py [frames] [interval]", file=sys.stderr)
        return 2
    try:
        frames = int(argv[0]) if argv else 7500
        interval = int(argv[1]) if len(argv) == 2 else 300
    except ValueError:
        print("oracle_lockstep: frames and interval must be integers", file=sys.stderr)
        return 2
    if frames <= 0 or interval <= 0:
        print("oracle_lockstep: frames and interval must be positive", file=sys.stderr)
        return 2
    try:
        environment = load_environment(REPO_ROOT)
        build = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
        output = reset_scratch_directory(REPO_ROOT / "scratch/oracle/lockstep")
        selected = load_profile(REPO_ROOT).navigation
        table = environment.get("GEARS_WALK_TABLE", selected.frame_walk)
        events = parse_frame_walk(table)
        ours = native_schedule(selected, table)
        theirs = oracle_schedule(selected, table)
        walk_last = last_frame(selected, table)
    except (
        BuildPathError,
        EnvironmentError,
        ProfileError,
        ReplayCorpusError,
    ) as error:
        print(f"oracle_lockstep: REFUSING: {error}", file=sys.stderr)
        return 2
    if not ours or not theirs:
        print("oracle_lockstep: REFUSING: generated walk is empty", file=sys.stderr)
        return 2
    if frames <= walk_last:
        print(
            f"oracle_lockstep: REFUSING: {frames} frames do not finish the walk at {walk_last}",
            file=sys.stderr,
        )
        return 2
    game = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
    executable = game / "default.xex"
    runtime = build / "runtime/gears1"
    oracle = REPO_ROOT / "build/oracle/xenia_oracle"
    for required in (executable, runtime, oracle):
        if not required.is_file():
            print(f"oracle_lockstep: REFUSING: missing {required}", file=sys.stderr)
            return 2
    iso = environment.get("GEARS_ISO")
    if iso and Path(iso).is_file():
        oracle_target = Path(iso)
        oracle_source = "the disc image"
    else:
        oracle_target = executable
        oracle_source = "the extracted tree"
    for name in ("ours", "ours2", "theirs"):
        (output / name).mkdir()
    ours_timeout = frames // 30 + 180
    theirs_timeout = frames // 30 + 300
    print(f"walk: {len(events)} events from one checked title-profile table")
    print("== native renderer, run 1 ==")
    _native_run(
        runtime,
        executable,
        game,
        output / "ours",
        output / "ours.log",
        environment,
        ours,
        frames,
        interval,
        ours_timeout,
    )
    print("== native renderer, run 2 (determinism control) ==")
    _native_run(
        runtime,
        executable,
        game,
        output / "ours2",
        output / "ours2.log",
        environment,
        ours,
        frames,
        interval,
        ours_timeout,
    )
    print("== oracle renderer, frame-driven ==")
    _oracle_run(
        oracle,
        oracle_target,
        output / "theirs",
        output / "theirs.log",
        environment,
        theirs,
        frames,
        interval,
        theirs_timeout,
    )
    counts = {
        "ours": len(tuple((output / "ours").glob("*.ppm"))),
        "ours2": len(tuple((output / "ours2").glob("*.ppm"))),
        "theirs": len(tuple((output / "theirs").glob("*.png"))),
    }
    expected = frames // interval
    manifest_lines = [
        "indexed by: guest VdSwap frame counter on both sides",
        f"walk table: {table}",
        f"walk (ours): {ours}",
        f"walk (theirs): {theirs}",
        f"oracle booted from: {oracle_source}",
        f"ours: {counts['ours']} frames; ours2: {counts['ours2']}; theirs: {counts['theirs']}",
        f"expected on each side: {expected}",
        "",
        "Frame N is comparable across renderers only where ours and ours2 prove the title deterministic.",
    ]
    for side, count in counts.items():
        if count < expected:
            manifest_lines.append(
                f"SHORT: {side} wrote {count} of {expected} frames and did not finish"
            )
    manifest = "\n".join(manifest_lines) + "\n"
    print(manifest, end="")
    (output / "manifest.txt").write_text(manifest, encoding="utf-8")
    if counts["ours"] == 0 or counts["theirs"] == 0:
        print("FAILED: a side produced no frames; there is nothing to compare.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
