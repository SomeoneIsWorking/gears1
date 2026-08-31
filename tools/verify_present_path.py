#!/usr/bin/env python3
"""Verify the real headless present path is pixel-identical to renderer output."""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

from gearsue3_bootstrap.process import terminate_child
from gearsue3_bootstrap.paths import BuildPathError, build_directory
from gearsue3_bootstrap.profile import load_profile
from replay_corpus import REPO_ROOT, ReplayCorpusError, ppm_pixels, reset_scratch_directory


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if len(argv) > 1:
        print("usage: tools/verify_present_path.py [frame]", file=sys.stderr)
        return 2
    try:
        frame = int(argv[0]) if argv else 400
    except ValueError:
        print("verify_present_path: frame must be an integer", file=sys.stderr)
        return 2
    if frame <= 0:
        print("verify_present_path: frame must be positive", file=sys.stderr)
        return 2

    environment = dict(os.environ)
    output = Path(
        environment.get("GEARS_VERIFY_DIR", REPO_ROOT / "scratch/verify/present")
    )
    game = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
    try:
        build = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
    except BuildPathError as error:
        print(f"verify_present_path: REFUSING: {error}", file=sys.stderr)
        return 2
    runtime = build / "runtime/gears1"
    executable = game / "default.xex"
    if not executable.is_file() or not runtime.is_file():
        print(
            f"verify_present_path: REFUSING: need {executable} and {runtime}",
            file=sys.stderr,
        )
        return 2
    try:
        reset_scratch_directory(output)
    except ReplayCorpusError as error:
        print(f"verify_present_path: REFUSING: {error}", file=sys.stderr)
        return 2
    navigation = load_profile(REPO_ROOT).navigation
    run_environment = {
        **environment,
        "GEARS_PRESENT_HEADLESS": "1",
        "GEARS_PRESENT_DUMP": "1",
        "GEARS_PRESENT_DUMP_AT": str(frame + 60),
        "GEARS_PRESENT_DUMP_DIR": str(output),
        "GEARS_DRAW_DIR": str(output),
        "GEARS_DRAW_FRAME_AT": str(frame),
        "GEARS_DRAW_FRAME_COUNT": "1",
        "GEARS_INPUT_SCRIPT": navigation.start_walk,
    }
    log_path = output / "run.log"
    with log_path.open("wb") as log:
        child = subprocess.Popen(
            [runtime, executable, game],
            cwd=REPO_ROOT,
            env=run_environment,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            deadline = time.monotonic() + 300
            while child.poll() is None and time.monotonic() < deadline:
                if (output / "presented_1.ppm").is_file() and (
                    output / "frame.ppm"
                ).is_file():
                    break
                time.sleep(0.25)
        finally:
            terminate_child(child)

    presented_path = output / "presented_1.ppm"
    rendered_path = output / "frame.ppm"
    if not presented_path.is_file() or not rendered_path.is_file():
        print(
            "verify_present_path: REFUSING: no complete presented/rendered pair; "
            f"see {log_path}",
            file=sys.stderr,
        )
        return 3
    try:
        presented_width, presented_height, presented = ppm_pixels(presented_path)
        rendered_width, rendered_height, rendered = ppm_pixels(rendered_path)
    except (OSError, ReplayCorpusError) as error:
        print(f"verify_present_path: REFUSING: {error}", file=sys.stderr)
        return 3
    if (presented_width, presented_height) != (rendered_width, rendered_height):
        print(
            f"FAIL: presented {presented_width}x{presented_height} but rendered "
            f"{rendered_width}x{rendered_height}"
        )
        return 1
    if presented == rendered:
        print(
            f"PASS: presented frame equals renderer output ({presented_width}x"
            f"{presented_height}, {len(presented)} bytes)"
        )
        return 0
    differences = [abs(left - right) for left, right in zip(presented, rendered)]
    print(
        f"FAIL: {sum(value != 0 for value in differences)} of {len(differences)} "
        f"bytes differ, worst by {max(differences)}"
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
