#!/usr/bin/env python3
"""Drive the title to a content mount and refuse unless the checkpoint is reached."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import time
from pathlib import Path

from gearsue3_bootstrap.process import terminate_child
from gearsue3_bootstrap.paths import BuildPathError, build_directory
from gearsue3_bootstrap.profile import ProfileError, load_profile
from replay_corpus import REPO_ROOT, ReplayCorpusError, environment_integer

_ENVIRONMENT_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _arguments(argv: list[str]) -> tuple[Path, dict[str, str], list[str]]:
    log = Path(argv.pop(0)) if argv and argv[0] != "--" else Path("scratch/logs/checkpoint.log")
    environment: dict[str, str] = {}
    runtime_arguments: list[str] = []
    after_separator = False
    for argument in argv:
        if argument == "--" and not after_separator:
            after_separator = True
            continue
        if after_separator:
            runtime_arguments.append(argument)
            continue
        name, separator, value = argument.partition("=")
        if separator == "" or _ENVIRONMENT_NAME.fullmatch(name) is None:
            raise ValueError(
                f"expected NAME=VALUE or -- before runtime arguments, got {argument!r}"
            )
        environment[name] = value
    return log, environment, runtime_arguments


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    try:
        log, overrides, runtime_arguments = _arguments(argv)
    except ValueError as error:
        print(f"run_to_checkpoint: {error}", file=sys.stderr)
        return 2
    environment = {**os.environ, **overrides}
    game = Path(environment.get("GEARS_GAME_DIR", REPO_ROOT / "scratch/game"))
    try:
        build = build_directory(
            REPO_ROOT,
            environment.get("GEARS_BUILD_DIR"),
            REPO_ROOT / "build/release",
        )
    except BuildPathError as error:
        print(f"run_to_checkpoint: REFUSING: {error}", file=sys.stderr)
        return 2
    binary = Path(environment.get("GEARS_BINARY", build / "runtime/gears1"))
    executable = game / "default.xex"
    if not executable.is_file() or not binary.is_file():
        print(
            f"run_to_checkpoint: REFUSING: need {executable} and {binary}",
            file=sys.stderr,
        )
        return 2
    if not log.is_absolute():
        log = REPO_ROOT / log
    log.parent.mkdir(parents=True, exist_ok=True)
    try:
        navigation = load_profile(REPO_ROOT).navigation
        post_mount = environment_integer(
            environment, "GEARS_POST_MOUNT_SECONDS", 90
        )
    except (ProfileError, ReplayCorpusError) as error:
        print(f"run_to_checkpoint: REFUSING: {error}", file=sys.stderr)
        return 2
    run_environment = {
        **environment,
        "GEARS_INPUT_SCRIPT": navigation.checkpoint_walk,
        "GEARS_NO_WINDOW": "1",
    }
    mounted = False
    with log.open("wb") as output:
        child = subprocess.Popen(
            [binary, executable, game, *runtime_arguments],
            cwd=REPO_ROOT,
            env=run_environment,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        print(child.pid)
        deadline = time.monotonic() + 300
        try:
            while child.poll() is None and time.monotonic() < deadline:
                output.flush()
                mounted = b"mounted as" in log.read_bytes()
                if mounted:
                    try:
                        child.wait(timeout=post_mount)
                    except subprocess.TimeoutExpired:
                        pass
                    break
                time.sleep(0.25)
        finally:
            terminate_child(child)
    mounted = mounted or b"mounted as" in log.read_bytes()
    if not mounted:
        print(
            "run_to_checkpoint: REFUSING: title never mounted save content; "
            "the maintained menu route did not reach its checkpoint",
            file=sys.stderr,
        )
        return 3
    mounts = log.read_bytes().count(b"mounted as")
    print(
        f"run_to_checkpoint: reached the checkpoint ({mounts} content mount(s))",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
