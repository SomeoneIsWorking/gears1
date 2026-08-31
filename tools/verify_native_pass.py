#!/usr/bin/env python3
"""A/B one native pass against translated microcode and its Vulkan interface."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

from replay_corpus import (
    REPO_ROOT,
    ReplayCorpusError,
    clear_frame_outputs,
    one_frame_output,
    replay_executable,
    reset_scratch_directory,
)


def _run_replay(
    replay: Path,
    capture: Path,
    log: Path,
    environment: dict[str, str],
) -> str:
    clear_frame_outputs()
    completed = subprocess.run(
        [replay, capture],
        cwd=REPO_ROOT,
        env={**os.environ, **environment},
        capture_output=True,
        text=True,
        check=False,
    )
    combined = completed.stdout + completed.stderr
    log.write_text(combined, encoding="utf-8")
    if completed.returncode != 0:
        raise ReplayCorpusError(
            f"replay of {capture} exited {completed.returncode}; see {log}"
        )
    return combined


def _arm(
    replay: Path,
    capture: Path,
    output: Path,
    native: bool,
    destination: Path,
) -> str:
    value = "1" if native else "0"
    log = output / f"arm{value}.log"
    combined = _run_replay(
        replay, capture, log, {"GEARS_NATIVE_PASSES": value}
    )
    if native and "is rendering natively" not in combined:
        raise ReplayCorpusError(
            f"no native pass was substituted in {capture}; both arms would run translated shaders"
        )
    shutil.copyfile(one_frame_output(), destination)
    return combined


def _compare(left: Path, right: Path, quiet: bool = False) -> int:
    return subprocess.run(
        [sys.executable, REPO_ROOT / "tools/compare_frames.py", left, right],
        cwd=REPO_ROOT,
        stdout=subprocess.DEVNULL if quiet else None,
        stderr=subprocess.DEVNULL if quiet else None,
        check=False,
    ).returncode


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if len(argv) > 2:
        print(
            "usage: tools/verify_native_pass.py [capture.gfr] [control.gfr]",
            file=sys.stderr,
        )
        return 2
    capture = Path(argv[0]) if argv else REPO_ROOT / "scratch/frames/act1_v2.gfr"
    control = Path(argv[1]) if len(argv) == 2 else REPO_ROOT / "scratch/frames/play_v2.gfr"
    if not capture.is_absolute():
        capture = REPO_ROOT / capture
    if not control.is_absolute():
        control = REPO_ROOT / control
    if not capture.is_file():
        print(f"verify_native_pass: REFUSING: no capture at {capture}", file=sys.stderr)
        return 2
    try:
        replay = replay_executable()
        output = reset_scratch_directory(REPO_ROOT / "scratch/ab")
        translated = output / "xlate.ppm"
        native = output / "native.ppm"
        print(f"== translated microcode vs native shader, on {capture} ==")
        _arm(replay, capture, output, False, translated)
        native_log = _arm(replay, capture, output, True, native)
        for line in native_log.splitlines():
            if "is rendering natively" in line:
                print(f"  substituted: {line}")
        status = _compare(translated, native)

        print("\n== interface: Vulkan validation, native passes on ==")
        validation_log = output / "validate.log"
        validation = _run_replay(
            replay,
            capture,
            validation_log,
            {"GEARS_DRAW_VALIDATE": "1", "GEARS_NATIVE_PASSES": "1"},
        )
        substitutions = validation.count("is rendering natively")
        if substitutions == 0:
            raise ReplayCorpusError(
                "validation run substituted no native pass and checked no native interface"
            )
        interface_lines = [
            line
            for line in validation.splitlines()
            if "VkImageViewType" in line or "OpTypeImage" in line
        ]
        print(
            f"  {substitutions} native substitutions under validation; "
            f"{len(interface_lines)} interface warnings"
        )
        if interface_lines:
            print("INTERFACE MISMATCH:", file=sys.stderr)
            for line in sorted(set(interface_lines))[:5]:
                print(line, file=sys.stderr)
            status = 1

        if control.is_file():
            print(f"\n== negative control: {control} must not match ==")
            control_output = output / "control.ppm"
            _arm(replay, control, output, False, control_output)
            if _compare(translated, control_output, quiet=True) == 0:
                raise ReplayCorpusError(
                    "comparison calls two different captures identical; passing A/B is untrustworthy"
                )
            print("negative control reports a difference")
        else:
            print(
                f"NO NEGATIVE CONTROL: {control} is missing; this run did not prove the comparator fires",
                file=sys.stderr,
            )
        return status
    except (OSError, ReplayCorpusError) as error:
        print(f"verify_native_pass: REFUSING: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
