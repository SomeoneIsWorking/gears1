#!/usr/bin/env python3
"""Capture one content-selected pass window from native and oracle renderers."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import time
from datetime import UTC, datetime
from pathlib import Path

from gearsue3_bootstrap.environment import EnvironmentError, load_environment
from gearsue3_bootstrap.paths import BuildPathError, build_directory
from gearsue3_bootstrap.process import terminate_child, terminate_process_group
from gearsue3_bootstrap.profile import (
    ProfileError,
    load_profile,
    native_schedule,
    oracle_schedule,
)
from replay_corpus import (
    REPO_ROOT,
    ReplayCorpusError,
    environment_integer,
    reset_scratch_directory,
)
from title_scenario import ScenarioError, is_direct_boot, startup_map_name

_NATIVE_FRAME = re.compile(r"guest-draw: frame ([0-9]+) is the capture")
_ORACLE_FRAME = re.compile(r"dumping every resolve of frame ([0-9]+)")


def _stamp(directory: Path, role: str, pair: str, selector: str) -> None:
    completed = subprocess.run(
        [
            sys.executable,
            REPO_ROOT / "tools/provenance.py",
            "stamp",
            directory,
            "--role",
            role,
            "--pair",
            pair,
            "--note",
            f"selector={selector}",
            "--note",
            "script=layer_capture.py",
        ],
        cwd=REPO_ROOT,
        check=False,
    )
    if completed.returncode != 0:
        raise ReplayCorpusError(f"provenance stamp failed for {directory}")


def _native_capture(
    runtime: Path,
    executable: Path,
    game: Path,
    output: Path,
    environment: dict[str, str],
    schedule: str,
    minimum_draws: int,
    after: int,
    timeout: int,
) -> None:
    run_environment = {
        **environment,
        "GEARS_NO_WINDOW": "1",
        "GEARS_INPUT_SCRIPT": schedule,
        "GEARS_DRAW_FRAME_MIN_DRAWS": str(minimum_draws),
        "GEARS_DRAW_FRAME_AFTER_GAMEPLAY": str(after),
        "GEARS_DRAW_FRAME_COUNT": "1",
        "GEARS_DRAW_RESOLVE_DUMP_EACH": "1",
        "GEARS_DRAW_DIAG": str(output / "ours/draws.tsv"),
        "GEARS_DRAW_DIR": str(output / "ours"),
    }
    log_path = output / "ours.log"
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
                log.flush()
                if b"frame screenshot written" in log_path.read_bytes():
                    break
                time.sleep(0.25)
        finally:
            terminate_child(child, 20)


def _oracle_capture(
    oracle: Path,
    target: Path,
    output: Path,
    environment: dict[str, str],
    schedule: str,
    direct_boot: bool,
    minimum_draws: int,
    after: int,
    window: int,
    timeout: int,
) -> None:
    run_environment = {
        **environment,
        "SDL_AUDIODRIVER": "dummy",
        "GEARS_ORACLE_RESOLVE_DUMP": str(output / "theirs"),
        "GEARS_ORACLE_DUMP_MIN_DRAWS": str(minimum_draws),
        "GEARS_ORACLE_DUMP_AFTER_GAMEPLAY": str(after),
        "GEARS_ORACLE_DUMP_FRAMES": str(window),
        "GEARS_ORACLE_DRAW_STREAM": str(output / "theirs_draws.tsv"),
        "GEARS_ORACLE_DRAW_ORDER": str(output / "theirs_order.tsv"),
    }
    command = [
        oracle,
        "--store_shaders=false",
        f"--target={target}",
        f"--oracle_out={output / 'theirs_frames'}",
        "--oracle_by_frame=true",
        f"--oracle_frames={timeout * 30}",
        "--oracle_frame_interval=1200",
        f"--oracle_frame_timeout={timeout}",
        f"--oracle_allow_no_input={'true' if direct_boot else 'false'}",
        f"--oracle_input={schedule}",
    ]
    log_path = output / "theirs.log"
    with log_path.open("wb") as log:
        child = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            env=run_environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        deadline = time.monotonic() + timeout
        first_dump_at: float | None = None
        try:
            while child.poll() is None and time.monotonic() < deadline:
                if first_dump_at is None and any((output / "theirs").glob("oracle_f*.bin")):
                    first_dump_at = time.monotonic()
                if first_dump_at is not None and time.monotonic() - first_dump_at >= 10 + window * 10:
                    break
                time.sleep(0.25)
        finally:
            terminate_process_group(child, 20)


def _last_match(pattern: re.Pattern[str], paths: list[Path]) -> int | None:
    matches: list[int] = []
    for path in paths:
        matches.extend(int(value) for value in pattern.findall(path.read_text(errors="replace")))
    return matches[-1] if matches else None


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if len(argv) > 2:
        print("usage: tools/layer_capture.py [seconds] [out-dir]", file=sys.stderr)
        return 2
    try:
        timeout = int(argv[0]) if argv else 420
    except ValueError:
        print("layer_capture: seconds must be an integer", file=sys.stderr)
        return 2
    if timeout <= 0:
        print("layer_capture: seconds must be positive", file=sys.stderr)
        return 2
    try:
        environment = load_environment(REPO_ROOT)
        build = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
        output_arg = Path(argv[1]) if len(argv) == 2 else REPO_ROOT / "scratch/layercap"
        output = reset_scratch_directory(
            output_arg if output_arg.is_absolute() else REPO_ROOT / output_arg
        )
        game = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
        map_name = startup_map_name(game)
        navigation = load_profile(REPO_ROOT).navigation
        table = environment.get("GEARS_WALK_TABLE", navigation.frame_walk)
        ours = "" if is_direct_boot(map_name) else native_schedule(navigation, table)
        theirs = "" if is_direct_boot(map_name) else oracle_schedule(navigation, table)
        minimum_draws = environment_integer(
            environment, "GEARS_LAYER_MIN_DRAWS", 400, minimum=1
        )
        after = environment_integer(environment, "GEARS_LAYER_AFTER", 300)
        window = environment_integer(
            environment, "GEARS_LAYER_ORACLE_FRAMES", 12, minimum=1
        )
        tolerance = environment_integer(
            environment, "GEARS_LAYER_FRAME_TOLERANCE", 4
        )
    except (
        BuildPathError,
        EnvironmentError,
        ProfileError,
        ReplayCorpusError,
        ScenarioError,
    ) as error:
        print(f"layer_capture: REFUSING: {error}", file=sys.stderr)
        return 2
    direct = is_direct_boot(map_name)
    game_executable = game / "default.xex"
    runtime = build / "runtime/gears1"
    oracle = REPO_ROOT / "build/oracle/xenia_oracle"
    for required in (game_executable, runtime, oracle):
        if not required.is_file():
            print(f"layer_capture: REFUSING: missing {required}", file=sys.stderr)
            return 2
    iso = environment.get("GEARS_ISO")
    oracle_target = (
        Path(iso)
        if environment.get("GEARS_LAYER_ISO", "0") == "1"
        and iso
        and Path(iso).is_file()
        else game_executable
    )
    (output / "ours").mkdir()
    (output / "theirs").mkdir()
    pair = f"layercap-{datetime.now(UTC).strftime('%Y%m%dT%H%M%SZ')}-{os.getpid()}"
    selector = f"after{after}_mindraws{minimum_draws}"
    try:
        _stamp(output / "ours", "ours", pair, selector)
        _stamp(output / "theirs", "theirs", pair, selector)
        print(
            f"selector: {after} frames after first >= {minimum_draws}-draw frame; "
            f"{timeout}s per side"
        )
        if direct:
            print(f"startup map {map_name!r}: both guests boot directly with no input")
        print("== native renderer ==")
        _native_capture(
            runtime,
            game_executable,
            game,
            output,
            environment,
            ours,
            minimum_draws,
            after,
            timeout,
        )
        print("== oracle renderer ==")
        _oracle_capture(
            oracle,
            oracle_target,
            output,
            environment,
            theirs,
            direct,
            minimum_draws,
            after,
            window,
            timeout,
        )
    except (OSError, ReplayCorpusError) as error:
        print(f"layer_capture: REFUSING: {error}", file=sys.stderr)
        return 2
    oracle_log = (output / "theirs.log").read_text(errors="replace")
    if any(marker in oracle_log.lower() for marker in ("device_lost", "graphics device lost", "context is lost")):
        print("layer_capture: REFUSING: oracle lost the Vulkan device", file=sys.stderr)
        return 3
    if not any((output / "theirs").glob("oracle_f*.bin")):
        print(
            "layer_capture: REFUSING: oracle produced no pass dumps; read theirs.log",
            file=sys.stderr,
        )
        return 3
    ours_frame = _last_match(_NATIVE_FRAME, [output / "ours.log"])
    theirs_frame = _last_match(_ORACLE_FRAME, [output / "theirs.log"])
    if ours_frame is None or theirs_frame is None:
        print(
            f"layer_capture: REFUSING: selected frame missing (ours={ours_frame}, theirs={theirs_frame})",
            file=sys.stderr,
        )
        return 3
    gap = abs(ours_frame - theirs_frame)
    print(
        f"frame selected: ours {ours_frame}, theirs {theirs_frame} "
        f"(gap {gap}, tolerance {tolerance})"
    )
    if gap > tolerance:
        print("layer_capture: REFUSING: captures are different game moments", file=sys.stderr)
        return 3
    return subprocess.run(
        [
            sys.executable,
            REPO_ROOT / "tools/layer_compare.py",
            "--ours",
            output / "ours",
            "--theirs",
            output / "theirs",
            "--out",
            output / "layers",
        ],
        cwd=REPO_ROOT,
        check=False,
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
