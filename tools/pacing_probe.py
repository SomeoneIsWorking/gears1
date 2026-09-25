#!/usr/bin/env python3
"""Measure how fast Gears 1's world runs against wall time under one pacing.

Attach to an offscreen product serving its control channel, after its walk has
reached gameplay. The probe holds the left stick forward and samples the local
player's pawn, the world's game clock, and the present count, then reports
each per wall second. Run it once under the product's pacing and once under
`run_offscreen.py --console-pacing`: a world that runs at the console's speed
covers the same distance per wall second under both.

    uv run --locked python tools/run_offscreen.py --walk continue --seconds 400 \\
        --control-port 32125 [--console-pacing]
    uv run --locked python tools/pacing_probe.py --port 32125
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass

from product_control import ControlError, ProductControl

FULL_FORWARD = "32767"
POLL_SECONDS = 0.25
READY_TIMEOUT_SECONDS = 600.0


@dataclass(frozen=True)
class Sample:
    wall: float
    world: float
    presents: int
    location: tuple[float, float, float]


def take_sample(control: ProductControl) -> Sample:
    reading = control.player()
    pawn = reading["pawn"]
    if not isinstance(pawn, dict):
        raise ControlError("the local player has no pawn")
    status = control.status()
    x, y, z = (float(axis) for axis in pawn["location"])
    return Sample(time.monotonic(), float(reading["world_seconds"]), int(status["presents"]),
                  (x, y, z))


def wait_for_pad(control: ProductControl, deadline: float) -> None:
    """Wait until the walk has released the pad and a pawn exists."""
    last_error = "nothing polled"
    while time.monotonic() < deadline:
        try:
            control.set_pad({"ly": "0"})
            take_sample(control)
            return
        except ControlError as error:
            last_error = str(error)
            time.sleep(1.0)
    raise ControlError(f"the product never offered gameplay input: {last_error}")


def measure(control: ProductControl, seconds: float) -> dict[str, float]:
    control.set_pad({"ly": FULL_FORWARD})
    # Let the pawn reach its running speed before timing.
    time.sleep(1.0)
    samples = [take_sample(control)]
    end = samples[0].wall + seconds
    while samples[-1].wall < end:
        time.sleep(POLL_SECONDS)
        samples.append(take_sample(control))
    control.release()
    first, last = samples[0], samples[-1]
    wall = last.wall - first.wall
    travelled = sum(math.dist(a.location[:2], b.location[:2])
                    for a, b in zip(samples, samples[1:]))
    return {
        "wall_seconds": round(wall, 2),
        "samples": len(samples),
        "presents_per_second": round((last.presents - first.presents) / wall, 1),
        "game_seconds_per_wall_second": round((last.world - first.world) / wall, 4),
        "units_per_wall_second": round(travelled / wall, 1),
        "units_per_game_second": round(travelled / max(last.world - first.world, 1e-6), 1),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--seconds", type=float, default=4.0)
    arguments = parser.parse_args(argv)
    control = ProductControl(arguments.port)
    try:
        wait_for_pad(control, time.monotonic() + READY_TIMEOUT_SECONDS)
        print(json.dumps(measure(control, arguments.seconds)))
    except ControlError as error:
        print(f"pacing_probe: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
