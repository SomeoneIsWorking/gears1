#!/usr/bin/env python3
"""Check that the product renders Gears 1's first idle view as stock Xenia does.

Runs the headless Xenia oracle (``build/oracle/xenia_oracle``) and then the
product offscreen, each for the same time on the profile's menu walk, which
leaves both standing idle in Act 1's cell block. It compares each side's last
two captures with ``tools/frame_parity.py`` and writes
``scratch/oracle_compare/report.json`` and a side-by-side ``pair.png``.

    uv run --locked python tools/oracle_compare.py

The two runs execute one after the other, never at once. Exit status is 0 when
the frames match, 1 when they do not, and 2 when a run failed or produced
nothing to compare.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools import run_offscreen
from tools.frame_parity import FrameParityError, compare_idle, load_frame
from tools.gearsue3_bootstrap.environment import environment_file, load_environment
from tools.gearsue3_bootstrap.process import terminate_child
from tools.gearsue3_bootstrap.profile import load_profile, oracle_timed_schedule
from tools.gearsue3_bootstrap.provision import prepare_title

ORACLE = Path("build/oracle/xenia_oracle")
RUN_ROOT = Path("scratch/oracle_compare")
# The menu walk's last press is at 120 s; both sides stand idle in the cell
# block well before 240 s.
DEFAULT_SECONDS = 240
CAPTURE_INTERVAL = 10
# The oracle ends itself after its run; this bounds a hang, not the run.
ORACLE_SHUTDOWN_GRACE = 60


def run_oracle(image: Path, schedule: str, seconds: int, run_root: Path) -> list[Path]:
    """Run the oracle into ``run_root/oracle``; its log stays in a file because it names the image."""

    frames = run_root / "oracle"
    frames.mkdir(parents=True, exist_ok=True)
    for stale in frames.glob("frame_*.png"):
        stale.unlink()
    command = [
        os.fspath(REPO_ROOT / ORACLE),
        f"--target={image}",
        f"--oracle_out={frames}",
        f"--oracle_storage={run_root / 'oracle-storage'}",
        f"--oracle_seconds={seconds}",
        f"--oracle_interval={CAPTURE_INTERVAL}",
        f"--oracle_input={schedule}",
        # The product signs in its local player; an unsigned oracle takes the
        # "not signed in" route and never reaches the same screens.
        "--oracle_gamertag=Player",
        "--store_shaders=false",
    ]
    environment = {**os.environ, "SDL_AUDIODRIVER": "dummy"}
    with (run_root / "oracle.log").open("wb") as log:
        process = subprocess.Popen(
            command, cwd=REPO_ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT
        )
        try:
            status = process.wait(timeout=seconds + ORACLE_SHUTDOWN_GRACE)
        except subprocess.TimeoutExpired:
            terminate_child(process)
            raise FrameParityError(
                f"the oracle did not end within {ORACLE_SHUTDOWN_GRACE} s of its run; "
                f"see {run_root / 'oracle.log'}"
            ) from None
    if status != 0:
        raise FrameParityError(f"the oracle exited {status}; see {run_root / 'oracle.log'}")
    return sorted(frames.glob("frame_*.png"))


def run_product(seconds: int) -> list[Path]:
    status = run_offscreen.main(
        ["--walk", "menu", "--seconds", str(seconds), "--capture-every", str(CAPTURE_INTERVAL)]
    )
    if status != 0:
        raise FrameParityError(f"the product run exited {status}")
    return run_offscreen.captured_frames(REPO_ROOT / run_offscreen.RUN_ROOT / "frames")


def last_two(frames: Sequence[Path], side: str) -> tuple[Path, Path]:
    if len(frames) < 2:
        raise FrameParityError(f"the {side} captured {len(frames)} frame(s); two are needed")
    return frames[-2], frames[-1]


def save_pair(oracle: Path, product: Path, output: Path) -> None:
    with Image.open(oracle) as left, Image.open(product) as right:
        pair = Image.new("RGB", (left.width + right.width, max(left.height, right.height)))
        pair.paste(left.convert("RGB"), (0, 0))
        pair.paste(right.convert("RGB"), (left.width, 0))
    pair.save(output)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=int, default=DEFAULT_SECONDS)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    if arguments.seconds < 2 * CAPTURE_INTERVAL:
        raise SystemExit(f"--seconds must be at least {2 * CAPTURE_INTERVAL}")
    if not (REPO_ROOT / ORACLE).is_file():
        raise SystemExit(
            f"oracle_compare: {ORACLE} is not built; build it as tools/xenia_oracle/CMakeLists.txt "
            "describes"
        )
    profile = load_profile(REPO_ROOT)
    selected = environment_file(REPO_ROOT)
    environment = load_environment(REPO_ROOT, env_file=selected)
    prepared = prepare_title(REPO_ROOT, profile, environ=environment, env_file=selected)
    schedule = oracle_timed_schedule(profile.navigation.menu_walk)

    run_root = REPO_ROOT / RUN_ROOT
    run_root.mkdir(parents=True, exist_ok=True)
    try:
        oracle = last_two(
            run_oracle(prepared.image, schedule, arguments.seconds, run_root), "oracle"
        )
        product = last_two(run_product(arguments.seconds), "product")
        parity = compare_idle(
            (load_frame(oracle[0]), load_frame(oracle[1])),
            (load_frame(product[0]), load_frame(product[1])),
        )
    except FrameParityError as error:
        print(f"oracle_compare: FAILED: {error}", file=sys.stderr)
        return 2
    save_pair(oracle[1], product[1], run_root / "pair.png")
    report = {
        "seconds": arguments.seconds,
        "oracle_frames": [path.name for path in oracle],
        "product_frames": [path.name for path in product],
        **parity.report(),
    }
    (run_root / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    verdict = "match" if parity.matches else "DIFFER"
    print(
        f"oracle_compare: frames {verdict}: difference {parity.difference:.3f} against limit "
        f"{parity.limit:.3f} (motion: oracle {parity.oracle_motion:.3f}, product "
        f"{parity.product_motion:.3f}); report {RUN_ROOT / 'report.json'}"
    )
    return 0 if parity.matches else 1


if __name__ == "__main__":
    raise SystemExit(main())
