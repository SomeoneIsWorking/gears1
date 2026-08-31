#!/usr/bin/env python3
"""Replay every capture with Vulkan validation and report every VUID."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

from replay_corpus import REPO_ROOT, ReplayCorpusError, captures, replay_executable

VUID = re.compile(r"VUID-[A-Za-z0-9-]+")


def main(arguments: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if arguments is None else arguments)
    if len(argv) > 1:
        print("usage: tools/validate_all.py [output.txt]", file=sys.stderr)
        return 2
    try:
        replay = replay_executable()
        corpus = captures()
    except ReplayCorpusError as error:
        print(f"validate_all: REFUSING: {error}", file=sys.stderr)
        return 2
    destination = Path(argv[0]) if argv else None
    lines = [f"# vulkan validation, {len(corpus)} capture(s); zero VUIDs allowed"]
    failed = False
    environment = {**os.environ, "GEARS_DRAW_VALIDATE": "1"}
    for capture in corpus:
        completed = subprocess.run(
            [replay, capture],
            cwd=REPO_ROOT,
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        combined = completed.stdout + completed.stderr
        vuids = Counter(VUID.findall(combined))
        if completed.returncode != 0:
            failed = True
            lines.append(f"{capture.name}\tREPLAY-FAILED({completed.returncode})")
        elif vuids:
            failed = True
            detail = " ".join(f"{name}x{count}" for name, count in sorted(vuids.items()))
            lines.append(f"{capture.name}\tUNEXPECTED\t{detail}")
        else:
            lines.append(f"{capture.name}\tok\tnone")
    lines.append(
        "# FAIL: at least one capture failed or raised a VUID"
        if failed
        else "# PASS: every capture was Vulkan-validation clean"
    )
    report = "\n".join(lines) + "\n"
    sys.stdout.write(report)
    if destination is not None:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(report, encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
