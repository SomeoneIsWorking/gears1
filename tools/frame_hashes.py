#!/usr/bin/env python3
"""Render every capture and report content hashes, failures, and black output."""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
from pathlib import Path

from replay_corpus import (
    REPO_ROOT,
    ReplayCorpusError,
    captures,
    clear_frame_outputs,
    one_frame_output,
    ppm_pixels,
    replay_executable,
)


class Reporter:
    def __init__(self, destination: Path | None) -> None:
        self.destination = destination
        if destination is not None:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text("", encoding="utf-8")

    def line(self, value: str) -> None:
        print(value)
        if self.destination is not None:
            with self.destination.open("a", encoding="utf-8") as output:
                output.write(value + "\n")


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if len(argv) > 1:
        print("usage: tools/frame_hashes.py [output.txt]", file=sys.stderr)
        return 2
    try:
        replay = replay_executable()
        corpus = captures()
    except ReplayCorpusError as error:
        print(f"frame_hashes: REFUSING: {error}", file=sys.stderr)
        return 2
    reporter = Reporter(Path(argv[0]) if argv else None)
    reporter.line(f"# rendered-frame hashes, {len(corpus)} capture(s)")
    failed: list[str] = []
    black: list[str] = []
    for capture in corpus:
        try:
            clear_frame_outputs()
            completed = subprocess.run(
                [replay, capture],
                cwd=REPO_ROOT,
                env=dict(os.environ),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            if completed.returncode != 0:
                failed.append(capture.name)
                reporter.line(f"{capture.name}\tREPLAY-FAILED({completed.returncode})")
                continue
            output = one_frame_output()
            _width, _height, pixels = ppm_pixels(output)
            digest = hashlib.sha256(output.read_bytes()).hexdigest()[:16]
            if any(pixels):
                reporter.line(f"{capture.name}\t{digest}")
            else:
                black.append(capture.name)
                reporter.line(f"{capture.name}\t{digest}\tALL-BLACK")
        except (OSError, ReplayCorpusError) as error:
            failed.append(capture.name)
            reporter.line(f"{capture.name}\tFAILED: {error}")
    if black:
        reporter.line(
            f"# {len(black)} of {len(corpus)} captures rendered completely black: "
            + " ".join(black)
        )
    if failed:
        reporter.line(
            f"# FAIL: {len(failed)} capture(s) produced no trustworthy hash: "
            + " ".join(failed)
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
