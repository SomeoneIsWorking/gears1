#!/usr/bin/env python3
"""Create one provenance-, camera-, input-, and UI-matched renderer pair."""

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
    last_frame,
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


def _run_tool(arguments: list[Path | str]) -> None:
    completed = subprocess.run(
        [sys.executable, *arguments], cwd=REPO_ROOT, check=False
    )
    if completed.returncode != 0:
        raise ReplayCorpusError(
            f"tool exited {completed.returncode}: {' '.join(map(str, arguments))}"
        )


def _remove_scratch_file(path: Path) -> None:
    scratch = (REPO_ROOT / "scratch").resolve()
    target = path.resolve()
    if scratch not in target.parents:
        raise ReplayCorpusError(f"file cleanup escapes scratch: {path}")
    if path.exists():
        if path.is_symlink() or not path.is_file():
            raise ReplayCorpusError(f"cleanup target is not a regular file: {path}")
        path.unlink()


def _oracle_capture(
    oracle: Path,
    executable: Path,
    output: Path,
    constants: Path,
    environment: dict[str, str],
    schedule: str,
    direct: bool,
    minimum_draws: int,
    after: int,
    walk_last: int,
    shader: str,
    timeout: int,
    wanted_frames: int,
) -> None:
    run_environment = {
        **environment,
        "SDL_AUDIODRIVER": "dummy",
        "GEARS_ORACLE_RESOLVE_DUMP": str(output / "theirs"),
        "GEARS_ORACLE_DUMP_MIN_DRAWS": str(minimum_draws),
        "GEARS_ORACLE_DUMP_MIN_GUEST_FRAME": str(walk_last),
        "GEARS_ORACLE_DUMP_AFTER_GAMEPLAY": str(after),
        "GEARS_ORACLE_DUMP_FRAMES": str(wanted_frames),
        "GEARS_ORACLE_DRAW_ORDER": str(output / "theirs_order.tsv"),
        "GEARS_ORACLE_VS_CONSTS": shader,
        "GEARS_ORACLE_VS_CONSTS_ALL": environment.get("VS_CONSTS_ALL", ""),
        "GEARS_ORACLE_VS_CONSTS_ALL_OUT": str(output / "theirs_vs_consts_all.txt"),
        "GEARS_ORACLE_VDUMP_VS": environment.get("VDUMP_VS", ""),
        "GEARS_ORACLE_VDUMP_VS_OUT": str(output / "theirs_geometry.txt"),
        "GEARS_ORACLE_PRIM_STATS": environment.get("PRIM_STATS", ""),
    }
    command = [
        oracle,
        "--store_shaders=false",
        f"--target={executable}",
        f"--oracle_out={output / 'theirs_frames'}",
        "--oracle_by_frame=true",
        f"--oracle_frames={timeout * 30}",
        "--oracle_frame_interval=1200",
        f"--oracle_frame_timeout={timeout}",
        f"--oracle_allow_no_input={'true' if direct else 'false'}",
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
        try:
            while child.poll() is None and time.monotonic() < deadline:
                dumped = sum("_f6_e0_" in path.name for path in (output / "theirs").iterdir())
                if dumped >= wanted_frames:
                    print(f"console dumped {dumped}/{wanted_frames} frames")
                    break
                time.sleep(0.25)
        finally:
            terminate_process_group(child, 20)
    if not constants.is_file() or constants.stat().st_size == 0:
        raise ReplayCorpusError(
            f"oracle emitted no constants for shader {shader}; this run measured nothing"
        )


def _native_capture(
    runtime: Path,
    executable: Path,
    game: Path,
    output: Path,
    environment: dict[str, str],
    schedule: str,
    minimum_draws: int,
    after: int,
    walk_last: int,
    shader: str,
    camera: Path,
    near: str,
    rotation_near: str,
    constant_base: int,
    timeout: int,
) -> None:
    run_environment = {
        **environment,
        "GEARS_NO_WINDOW": "1",
        "GEARS_DRAW_FRAME_MIN_DRAWS": str(minimum_draws),
        "GEARS_DRAW_FRAME_MIN_GUEST_FRAME": str(walk_last),
        "GEARS_DRAW_FRAME_AFTER_GAMEPLAY": str(after),
        "GEARS_DRAW_FRAME_COUNT": "1",
        "GEARS_DRAW_FRAME_NEEDS": shader,
        "GEARS_DRAW_FRAME_CAMERA": f"{camera}:{near}:{rotation_near}:{constant_base}",
        "GEARS_DRAW_VS_CONSTS_VS": environment.get("VS_CONSTS_ALL", ""),
        "GEARS_DRAW_VDUMP_VS": environment.get("VDUMP_VS", ""),
        "GEARS_DRAW_RESOLVE_DUMP_EACH": "1",
        "GEARS_DRAW_DIAG": str(output / "ours/draws.tsv"),
        "GEARS_DRAW_DIR": str(output / "ours"),
        "GEARS_INPUT_SCRIPT": schedule,
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


def _validate_oracle_input(log: str, direct: bool) -> None:
    schedules = len(re.findall(r"oracle: [0-9]+ scheduled press\(es\)", log))
    frame_driven = log.count(
        "oracle: input and captures are driven by the GUEST FRAME COUNTER"
    )
    no_input = log.count("oracle: NO input schedule, by request")
    valid = (no_input == 1 and schedules == 0) if direct else (
        schedules == 1 and frame_driven == 1
    )
    if not valid:
        raise ReplayCorpusError(
            f"oracle input validation failed: direct={direct}, schedules={schedules}, "
            f"frame_driven={frame_driven}, no_input={no_input}"
        )


def _validate_native_input(log: str, direct: bool) -> None:
    sources = log.count("[input] scripted input:")
    steps = log.count("[input] scripted pad at ")
    if direct and (sources != 0 or steps != 0):
        raise ReplayCorpusError("direct-boot native run unexpectedly used scripted input")
    if not direct and (sources != 1 or steps < 1):
        raise ReplayCorpusError(
            f"native input validation failed: {sources} source declaration(s), {steps} step(s)"
        )
    selectors = log.count("[xam] storage device selected automatically:")
    expected = 0 if direct else 1
    if selectors != expected:
        raise ReplayCorpusError(
            f"native logged {selectors} automatic storage selections; expected {expected}"
        )


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if len(argv) > 3:
        print(
            "usage: tools/camera_pair.py [seconds-per-side] [out-dir] [vs-hash]",
            file=sys.stderr,
        )
        return 2
    try:
        timeout = int(argv[0]) if argv else 300
    except ValueError:
        print("camera_pair: seconds must be an integer", file=sys.stderr)
        return 2
    if timeout <= 0:
        print("camera_pair: seconds must be positive", file=sys.stderr)
        return 2
    try:
        environment = load_environment(REPO_ROOT)
        build = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
        output_arg = Path(argv[1]) if len(argv) >= 2 else REPO_ROOT / "scratch/camerapair"
        output = reset_scratch_directory(
            output_arg if output_arg.is_absolute() else REPO_ROOT / output_arg
        )
        game = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
        map_name = startup_map_name(game)
        selected = load_profile(REPO_ROOT).navigation
        table = environment.get(
            "GEARS_CAMERA_PAIR_WALK_TABLE", selected.camera_pair_frame_walk
        )
        ours = "" if is_direct_boot(map_name) else native_schedule(selected, table)
        theirs = "" if is_direct_boot(map_name) else oracle_schedule(selected, table)
        walk_last = 0 if is_direct_boot(map_name) else last_frame(selected, table)
        minimum_draws = environment_integer(
            environment, "GEARS_LAYER_MIN_DRAWS", 400, minimum=1
        )
        after = environment_integer(environment, "GEARS_LAYER_AFTER", 300)
        constant_base = environment_integer(
            environment, "CAMERA_CONST_BASE", 230, maximum=252
        )
        wanted_frames = environment_integer(
            environment, "GEARS_ORACLE_DUMP_FRAMES", 5, minimum=1
        )
    except (
        BuildPathError,
        EnvironmentError,
        ProfileError,
        ReplayCorpusError,
        ScenarioError,
    ) as error:
        print(f"camera_pair: REFUSING: {error}", file=sys.stderr)
        return 2
    direct = is_direct_boot(map_name)
    shader = argv[2] if len(argv) == 3 else "f3e9368c1bb68ecc"
    near = environment.get("CAMERA_NEAR", "0.013")
    rotation_near = environment.get("CAMERA_ROT_NEAR", "0.005")
    executable = game / "default.xex"
    runtime = build / "runtime/gears1"
    oracle = REPO_ROOT / "build/oracle/xenia_oracle"
    for required in (executable, runtime, oracle):
        if not required.is_file():
            print(f"camera_pair: REFUSING: missing {required}", file=sys.stderr)
            return 2
    (output / "ours").mkdir()
    (output / "theirs").mkdir()
    constants = REPO_ROOT / "scratch/oracle/vs_consts.txt"
    pair = f"camerapair-{datetime.now(UTC).strftime('%Y%m%dT%H%M%SZ')}-{os.getpid()}"
    try:
        constants.parent.mkdir(parents=True, exist_ok=True)
        _remove_scratch_file(constants)
        print(f"== oracle: pass window and constants for shader {shader} ==")
        _oracle_capture(
            oracle,
            executable,
            output,
            constants,
            environment,
            theirs,
            direct,
            minimum_draws,
            after,
            walk_last,
            shader,
            timeout,
            wanted_frames,
        )
        oracle_log = (output / "theirs.log").read_text(errors="replace")
        _validate_oracle_input(oracle_log, direct)
        if any(
            marker in oracle_log.lower()
            for marker in ("device_lost", "graphics device lost", "context is lost")
        ):
            raise ReplayCorpusError("oracle lost the Vulkan device")
        constants_text = constants.read_text(errors="replace")
        frame_match = re.search(r"at guest frame ([0-9]+)", constants_text)
        if frame_match is None:
            raise ReplayCorpusError("camera constants do not identify their guest frame")
        camera_frame = int(frame_match.group(1))
        if not any(f"_f{camera_frame}_" in path.name for path in (output / "theirs").iterdir()):
            raise ReplayCorpusError(
                f"camera frame {camera_frame} has no oracle pass dump in the captured window"
            )
        missing_rows = [
            index
            for index in range(constant_base, constant_base + 4)
            if re.search(rf"(?m)^c\[{index}\]", constants_text) is None
        ]
        if missing_rows:
            raise ReplayCorpusError(f"camera constants omit rows {missing_rows}")
        common_stamp = [
            "--camera",
            constants,
            "--note",
            f"vs={shader}",
            "--note",
            "script=camera_pair.py",
            "--note",
            f"walk_table={table}",
            "--note",
            f"direct_boot={int(direct)}",
        ]
        _run_tool(
            [
                REPO_ROOT / "tools/provenance.py",
                "stamp",
                output / "theirs",
                "--role",
                "theirs",
                "--pair",
                pair,
                *common_stamp,
            ]
        )
        _run_tool(
            [
                REPO_ROOT / "tools/provenance.py",
                "stamp",
                output / "ours",
                "--role",
                "ours",
                "--pair",
                pair,
                *common_stamp,
                "--note",
                f"camera_near={near}",
                "--note",
                f"camera_rot_near={rotation_near}",
                "--note",
                f"camera_const_base={constant_base}",
            ]
        )
        frozen = output / "ours/camera.txt"
        print("== native renderer, gated on oracle viewpoint ==")
        _native_capture(
            runtime,
            executable,
            game,
            output,
            environment,
            ours,
            minimum_draws,
            after,
            walk_last,
            shader,
            frozen,
            near,
            rotation_near,
            constant_base,
            timeout,
        )
        native_log = (output / "ours.log").read_text(errors="replace")
        _validate_native_input(native_log, direct)
        if "device_lost" in native_log.lower() or "graphics device lost" in native_log.lower():
            raise ReplayCorpusError("native renderer lost the Vulkan device")
        if "CAMERA MATCHED" not in native_log:
            raise ReplayCorpusError("camera gate never matched; this run captured nothing")
        _run_tool(
            [
                REPO_ROOT / "tools/ui_state_check.py",
                output / "ours/draws.tsv",
                "--oracle",
                output / "theirs_order.tsv",
            ]
        )
        _run_tool(
            [
                REPO_ROOT / "tools/provenance.py",
                "check",
                output / "ours",
                output / "theirs",
            ]
        )
    except (OSError, ReplayCorpusError) as error:
        print(f"camera_pair: REFUSING: {error}", file=sys.stderr)
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
