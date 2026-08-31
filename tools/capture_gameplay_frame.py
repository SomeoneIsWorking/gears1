#!/usr/bin/env python3
"""Walk into Act 1 and capture gameplay through the shipping renderer path."""

from __future__ import annotations

import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tools.gearsue3_bootstrap.process import run_for_duration  # noqa: E402
from tools.gearsue3_bootstrap.paths import BuildPathError, build_directory  # noqa: E402
from tools.gearsue3_bootstrap.profile import load_profile  # noqa: E402


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    try:
        duration = int(argv.pop(0)) if argv else 180
    except ValueError:
        print("capture_gameplay_frame: duration must be an integer number of seconds", file=sys.stderr)
        return 2
    if duration <= 0:
        print("capture_gameplay_frame: duration must be positive", file=sys.stderr)
        return 2

    environment = dict(os.environ)
    navigation = load_profile(REPO_ROOT).navigation
    defaults = {
        "GEARS_DRAW_FRAME_AT": "1500",
        "GEARS_DRAW_FRAME_COUNT": "0",
        "GEARS_DRAW_FRAME_REPORT_EVERY": "60",
        "GEARS_NO_WINDOW": "1",
        "GEARS_INPUT_SCRIPT": environment.get(
            "GEARS_MENU_WALK", navigation.menu_walk
        ),
    }
    for name, value in defaults.items():
        environment.setdefault(name, value)

    try:
        build_dir = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
    except BuildPathError as error:
        print(f"capture_gameplay_frame: REFUSING: {error}", file=sys.stderr)
        return 2
    game_dir = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
    runtime = build_dir / "runtime/gears1"
    executable = game_dir / "default.xex"
    if not runtime.is_file() or not os.access(runtime, os.X_OK):
        print(f"capture_gameplay_frame: REFUSING: runtime is not built: {runtime}", file=sys.stderr)
        return 2
    if not executable.is_file():
        print(f"capture_gameplay_frame: REFUSING: title executable is missing: {executable}", file=sys.stderr)
        return 2

    return run_for_duration(
        [runtime, executable, game_dir, *argv],
        cwd=REPO_ROOT,
        environ=environment,
        duration_seconds=duration,
    )


if __name__ == "__main__":
    raise SystemExit(main())
