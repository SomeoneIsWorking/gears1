#!/usr/bin/env python3
"""Find captures that submit a skinned-character draw."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from replay_corpus import REPO_ROOT, ReplayCorpusError, captures, replay_executable


def verdict(replay: Path, capture: Path) -> str:
    completed = subprocess.run(
        [replay, capture],
        cwd=REPO_ROOT,
        env={**os.environ, "GEARS_SKINNED_CHECK": "1"},
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return {0: "FOUND", 2: "UNAVAILABLE", 3: "NONE"}.get(
        completed.returncode, f"ERROR({completed.returncode})"
    )


def listing(replay: Path, capture: Path) -> list[str]:
    completed = subprocess.run(
        [replay, capture],
        cwd=REPO_ROOT,
        env={
            **os.environ,
            "GEARS_SKINNED_CHECK": "1",
            "GEARS_SKINNED_CHECK_LIST": "1",
        },
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode not in {0, 3}:
        raise ReplayCorpusError(
            f"frame_replay returned {completed.returncode} while listing {capture}"
        )
    lines = [
        line
        for line in (completed.stdout + completed.stderr).splitlines()
        if "skinned draw" in line or "no skinned draw" in line
    ]
    if not lines:
        raise ReplayCorpusError(
            f"detector produced no positive or negative line for {capture}"
        )
    return lines


def selftest(replay: Path) -> int:
    positive = REPO_ROOT / "scratch/frames/bright.gfr"
    negative = REPO_ROOT / "scratch/frames/courtyard.gfr"
    for capture in (positive, negative):
        if not capture.is_file():
            raise ReplayCorpusError(
                f"{capture} is missing; this self-test would check nothing"
            )
    positive_verdict = verdict(replay, positive)
    negative_verdict = verdict(replay, negative)
    positive_lines = listing(replay, positive)
    negative_lines = listing(replay, negative)
    print(f"positive case {positive.name}: expected FOUND, got {positive_verdict}")
    print(f"negative case {negative.name}: expected NONE, got {negative_verdict}")
    passed = (
        positive_verdict == "FOUND"
        and negative_verdict == "NONE"
        and any("skinned draw" in line for line in positive_lines)
        and any("no skinned draw" in line for line in negative_lines)
    )
    print(
        "PASS -- detector answers both ways and scans 2/2"
        if passed
        else "FAIL -- detector is not discriminating"
    )
    return 0 if passed else 1


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    mode = argv[0] if argv else "table"
    if len(argv) > 1 or mode not in {"table", "--list", "--selftest"}:
        print("usage: tools/skinned_frames.py [--list|--selftest]", file=sys.stderr)
        return 2
    try:
        replay = replay_executable()
        if mode == "--selftest":
            return selftest(replay)
        corpus = captures()
        found = 0
        for capture in corpus:
            result = verdict(replay, capture)
            if result == "FOUND":
                found += 1
            print(f"{capture.name:<22} {result}")
            if mode == "--list":
                for line in listing(replay, capture):
                    print(f"    {line}")
        print(f"{found} of {len(corpus)} captures submit a skinned character mesh.")
        print("NOTE: FOUND means submitted, not necessarily visible.")
        return 0
    except ReplayCorpusError as error:
        print(f"skinned_frames: REFUSING: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
