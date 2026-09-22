#!/usr/bin/env python3
"""Drive the Gears 1 product headless: no window, silent, scripted input.

This is the maintainer path for observing and measuring the shipping product.
It provisions exactly as ``./run.sh`` does, then runs the same executable with
``--offscreen`` under a fixed ``scratch/offscreen/`` tree: an isolated storage
root (never the player's saves), the run log, per-interval guest-output
captures, and a contact sheet of them.

    uv run --locked python tools/run_offscreen.py --seconds 305 --walk gameplay

The run observes; it does not gate. Its exit status is the product's: nonzero
when the title never presented a frame.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections.abc import Sequence
from pathlib import Path

from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.gearsue3_bootstrap.environment import environment_file, load_environment
from tools.gearsue3_bootstrap.process import run_logged_child
from tools.gearsue3_bootstrap.profile import Navigation, load_profile
from tools.gearsue3_bootstrap.provision import prepare_title

RUN_ROOT = Path("scratch/offscreen")
SHEET_COLUMNS = 3
SHEET_CELL = (426, 240)


def walk_script(navigation: Navigation, walk: str) -> str:
    """The named navigation route from the title profile, or no input at all."""

    routes = {
        "none": "",
        "start": navigation.start_walk,
        "menu": navigation.menu_walk,
        "checkpoint": navigation.checkpoint_walk,
        "gameplay": navigation.gameplay_walk,
    }
    if walk not in routes:
        raise ValueError(f"unknown walk {walk!r}; choose one of {', '.join(routes)}")
    return routes[walk]


def contact_sheet(frames: Sequence[Path], output: Path) -> None:
    """Tile the captures in capture order; refuse an empty set rather than print nothing."""

    if not frames:
        raise RuntimeError("the run captured no guest output; there is no sheet to build")
    rows = (len(frames) + SHEET_COLUMNS - 1) // SHEET_COLUMNS
    sheet = Image.new("RGB", (SHEET_CELL[0] * SHEET_COLUMNS, SHEET_CELL[1] * rows))
    for index, frame in enumerate(frames):
        with Image.open(frame) as image:
            cell = image.convert("RGB").resize(SHEET_CELL)
        sheet.paste(
            cell,
            ((index % SHEET_COLUMNS) * SHEET_CELL[0], (index // SHEET_COLUMNS) * SHEET_CELL[1]),
        )
    sheet.save(output)


def captured_frames(directory: Path) -> list[Path]:
    return sorted(directory.glob("second-*.ppm"), key=lambda path: int(path.stem.split("-")[1]))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--iso", help="disc image or 7z archive (default: as ./run.sh)")
    parser.add_argument("--seconds", type=int, default=120)
    parser.add_argument("--capture-every", type=int, default=15)
    route = parser.add_mutually_exclusive_group()
    route.add_argument("--walk", default="menu", help="none, start, menu, checkpoint, or gameplay")
    route.add_argument(
        "--script", help="an explicit input script in the runtime's step grammar, e.g. 9000:LY+"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    if arguments.seconds <= 0 or arguments.capture_every <= 0:
        raise SystemExit("--seconds and --capture-every must be positive")
    profile = load_profile(REPO_ROOT)
    selected = environment_file(REPO_ROOT)
    environment = load_environment(REPO_ROOT, env_file=selected)
    prepared = prepare_title(
        REPO_ROOT, profile, image=arguments.iso, environ=environment, env_file=selected
    )

    run_root = REPO_ROOT / RUN_ROOT
    frames = run_root / "frames"
    frames.mkdir(parents=True, exist_ok=True)
    for stale in captured_frames(frames):
        stale.unlink()
    child_environment = dict(environment)
    child_environment["GEARS_INPUT_SCRIPT"] = (
        arguments.script
        if arguments.script is not None
        else walk_script(profile.navigation, arguments.walk)
    )
    command = prepared.command() + [
        "--offscreen",
        "--storage-root",
        os.fspath(run_root / "storage"),
        "--seconds",
        str(arguments.seconds),
        "--capture-dir",
        os.fspath(frames),
        "--capture-every",
        str(arguments.capture_every),
    ]
    status = run_logged_child(
        command, cwd=REPO_ROOT, environ=child_environment, log_path=run_root / "run.log"
    )
    sheet = run_root / "sheet.png"
    contact_sheet(captured_frames(frames), sheet)
    print(f"run_offscreen: status {status}; log {run_root / 'run.log'}; sheet {sheet}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
