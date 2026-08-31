#!/usr/bin/env python3
"""Run both depth models and a noise/default control at one frozen camera."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

from gearsue3_bootstrap.environment import EnvironmentError, load_environment
from gearsue3_bootstrap.paths import BuildPathError, build_directory
from gearsue3_bootstrap.process import terminate_child
from gearsue3_bootstrap.profile import load_profile, native_schedule
from replay_corpus import REPO_ROOT, ReplayCorpusError, reset_scratch_directory
from title_scenario import ScenarioError, is_direct_boot, startup_map_name


def _camera_policy(pair: Path, game: Path) -> tuple[str, str]:
    provenance_path = pair / "ours/PROVENANCE.json"
    try:
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        notes = provenance["notes"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise ReplayCorpusError(f"cannot read camera provenance {provenance_path}: {error}") from error
    near = str(notes.get("camera_near", "0.013"))
    rotation = str(notes.get("camera_rot_near", "0.005"))
    base = str(notes.get("camera_const_base", "230"))
    direct_note = notes.get("direct_boot")
    walk_table = notes.get("walk_table")
    if direct_note == "1":
        schedule = ""
    elif isinstance(walk_table, str) and walk_table:
        schedule = native_schedule(load_profile(REPO_ROOT).navigation, walk_table)
    else:
        map_name = startup_map_name(game)
        navigation = load_profile(REPO_ROOT).navigation
        schedule = "" if is_direct_boot(map_name) else native_schedule(
            navigation, navigation.camera_pair_frame_walk
        )
    return f"{near}:{rotation}:{base}", schedule


def _run_arm(
    name: str,
    split: str | None,
    runtime: Path,
    executable: Path,
    game: Path,
    frozen_camera: Path,
    camera_policy: str,
    schedule: str,
    output: Path,
    environment: dict[str, str],
    shader: str,
    timeout: int,
) -> None:
    destination = output / name
    destination.mkdir()
    run_environment = dict(environment)
    if split is None:
        run_environment.pop("GEARS_DRAW_SPLIT_DEPTH", None)
    else:
        run_environment["GEARS_DRAW_SPLIT_DEPTH"] = split
    run_environment.update(
        {
            "GEARS_NO_WINDOW": "1",
            "GEARS_DRAW_FRAME_MIN_DRAWS": environment.get(
                "GEARS_LAYER_MIN_DRAWS", "600"
            ),
            "GEARS_DRAW_FRAME_AFTER_GAMEPLAY": "0",
            "GEARS_DRAW_FRAME_COUNT": "1",
            "GEARS_DRAW_FRAME_NEEDS": shader,
            "GEARS_DRAW_FRAME_CAMERA": f"{frozen_camera}:{camera_policy}",
            "GEARS_DRAW_RESOLVE_DUMP_EACH": "1",
            "GEARS_DRAW_DIAG": str(destination / "draws.tsv"),
            "GEARS_DRAW_DIR": str(destination),
            "GEARS_INPUT_SCRIPT": schedule,
        }
    )
    log_path = output / f"{name}.log"
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
    log_text = log_path.read_text(errors="replace")
    if "device_lost" in log_text.lower() or "graphics device lost" in log_text.lower():
        raise ReplayCorpusError(f"arm {name!r} lost the Vulkan device")
    if "CAMERA MATCHED" not in log_text:
        raise ReplayCorpusError(
            f"arm {name!r} never matched the frozen camera in {timeout}s"
        )


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if not argv or len(argv) > 3:
        print(
            "usage: tools/depth_arm_ab_run.py <existing-pair-dir> [seconds] [vs-hash]",
            file=sys.stderr,
        )
        return 2
    pair = Path(argv[0])
    if not pair.is_absolute():
        pair = REPO_ROOT / pair
    try:
        timeout = int(argv[1]) if len(argv) >= 2 else 600
    except ValueError:
        print("depth_arm_ab_run: seconds must be an integer", file=sys.stderr)
        return 2
    shader = argv[2] if len(argv) == 3 else "f3e9368c1bb68ecc"
    frozen = pair / "ours/camera.txt"
    if not frozen.is_file() or not (pair / "theirs").is_dir():
        print("depth_arm_ab_run: REFUSING: pair has no frozen camera/console half", file=sys.stderr)
        return 2
    try:
        environment = load_environment(REPO_ROOT)
        build = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
        game = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
        camera_policy, schedule = _camera_policy(pair, game)
        output = reset_scratch_directory(pair / "ab")
    except (
        BuildPathError,
        EnvironmentError,
        ReplayCorpusError,
        ScenarioError,
    ) as error:
        print(f"depth_arm_ab_run: REFUSING: {error}", file=sys.stderr)
        return 2
    runtime = build / "runtime/gears1"
    executable = game / "default.xex"
    for required in (runtime, executable):
        if not required.is_file():
            print(f"depth_arm_ab_run: REFUSING: missing {required}", file=sys.stderr)
            return 2
    try:
        for name, split in (
            ("shared", "0"),
            ("split", "1"),
            ("control", "0"),
            ("default", None),
        ):
            print(f"== arm {name!r}: GEARS_DRAW_SPLIT_DEPTH={split or 'UNSET'} ==")
            _run_arm(
                name,
                split,
                runtime,
                executable,
                game,
                frozen,
                camera_policy,
                schedule,
                output,
                environment,
                shader,
                timeout,
            )
    except (OSError, ReplayCorpusError) as error:
        print(f"depth_arm_ab_run: REFUSING: {error}", file=sys.stderr)
        return 3
    return subprocess.run(
        [
            sys.executable,
            REPO_ROOT / "tools/depth_arm_ab.py",
            "--pair",
            pair,
            "--ab",
            output,
        ],
        cwd=REPO_ROOT,
        check=False,
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
