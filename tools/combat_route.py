#!/usr/bin/env python3
"""Play Gears 1 headless from a new campaign into Act 1's first firefight.

The profile's gameplay walk is fixed-time input, so where it leaves Marcus
varies from run to run. This route continues from there with closed-loop
steering: it reads the local player through the control channel
(``/api/player``) and turns the left stick toward each goal until the player
stands there, so every run reaches the same places:

    uv run --locked python tools/combat_route.py

It starts ``tools/run_offscreen.py --walk gameplay`` with a control port, waits
for the walk to hand over the pad, and then steps out of the cover the walk
ends in, answers the points-of-interest tutorial with Y, walks to the jammed
prison door, answers the objectives tutorial with LB, kicks the door open,
crosses the cell room to the yard, takes cover, and fires. Each tutorial holds
Marcus in place until its button is held. The report in ``scratch/combat_route/``
records where every step began and ended. The route fails, naming the step,
when a step does not reach its goal; it passes only when the player reached
cover in the yard and the weapon fired rounds there.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.product_control import ControlError, ProductControl

REPORT_ROOT = Path("scratch/combat_route")
STICK_LIMIT = 32767
YAW_UNITS_PER_TURN = 65536
POLL_SECONDS = 0.2
# A goal that moved the player less than this in STALL_SECONDS is blocked.
STALL_DISTANCE = 25.0
STALL_SECONDS = 2.5
# The walk takes about 300 s; the route about 120 s; the rest is margin.
RUN_SECONDS = 720
HANDOVER_TIMEOUT_SECONDS = 600
TUTORIAL_HOLD_SECONDS = 2.5
OBJECTIVES_CLOSE_SECONDS = 1.5
# A late prompt can need a second hold; a third stall is a real obstacle.
MAX_UNBLOCKS = 2


class RouteFailure(RuntimeError):
    """A step did not reach its goal."""


@dataclass(frozen=True)
class Player:
    """One reading of the local player; position is None while the player is dead."""

    position: tuple[float, float] | None
    control_yaw: int
    magazine_rounds_fired: int | None

    @staticmethod
    def from_json(reading: dict[str, object]) -> Player:
        pawn = reading["pawn"]
        if pawn is None:
            return Player(None, int(reading["control_yaw"]), None)
        location = pawn["location"]
        rounds = pawn["magazine_rounds_fired"]
        return Player(
            (float(location[0]), float(location[1])),
            int(reading["control_yaw"]),
            None if rounds is None else int(rounds),
        )


class Pad(Protocol):
    """The part of the control channel the route drives."""

    def player(self) -> dict[str, object]: ...

    def set_pad(self, fields: dict[str, str]) -> None: ...

    def release(self) -> None: ...


def stick_toward(position: tuple[float, float], control_yaw: int, goal: tuple[float, float]) -> tuple[int, int]:
    """Left-stick (lx, ly) that walks from position toward goal.

    Movement is relative to the view: full ly walks along the control yaw, and
    full lx walks a quarter turn clockwise of it, which is +y of +x in the
    engine's left-handed world.
    """

    heading = math.atan2(goal[1] - position[1], goal[0] - position[0])
    relative = heading - control_yaw * 2.0 * math.pi / YAW_UNITS_PER_TURN
    return round(STICK_LIMIT * math.sin(relative)), round(STICK_LIMIT * math.cos(relative))


def distance(a: tuple[float, float], b: tuple[float, float]) -> float:
    return math.hypot(a[0] - b[0], a[1] - b[1])


class Route:
    """Runs steps against a pad, recording where each began and ended."""

    def __init__(self, pad: Pad, clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep) -> None:
        self._pad = pad
        self._clock = clock
        self._sleep = sleep
        self.steps: list[dict[str, object]] = []

    def player(self) -> Player:
        return Player.from_json(self._pad.player())

    def alive(self, step: str) -> Player:
        player = self.player()
        if player.position is None:
            raise RouteFailure(f"{step}: the player died")
        return player

    def _record(self, step: str, begin: Player, **details: object) -> None:
        end = self.player()
        self.steps.append(
            {"step": step, "from": begin.position, "to": end.position, **details}
        )

    def hold(self, step: str, fields: dict[str, str], seconds: float) -> None:
        begin = self.alive(step)
        self._pad.set_pad(fields)
        self._sleep(seconds)
        self._pad.release()
        self._record(step, begin)

    def walk(self, step: str, goal: tuple[float, float], radius: float, timeout: float,
             unblock: Callable[[], None] | None = None, max_unblocks: int = MAX_UNBLOCKS) -> None:
        """Walk until within radius of goal.

        A stall runs unblock, when given, at most max_unblocks times: the
        tutorials hold the player in place until their button is held, and they
        trigger where the player walks, not where a step begins. Refuses when the
        player dies, stays stalled, or runs out of time.
        """

        begin = self.alive(step)
        start = self._clock()
        mark, mark_time = begin.position, start
        unblocks = 0
        try:
            while True:
                player = self.alive(step)
                if distance(player.position, goal) <= radius:
                    break
                now = self._clock()
                if now - start > timeout:
                    raise RouteFailure(
                        f"{step}: still {distance(player.position, goal):.0f} from "
                        f"{goal} after {timeout:.0f} s"
                    )
                if now - mark_time >= STALL_SECONDS:
                    if distance(player.position, mark) < STALL_DISTANCE:
                        if unblock is None or unblocks == max_unblocks:
                            raise RouteFailure(f"{step}: blocked at {player.position}")
                        self._pad.release()
                        unblock()
                        unblocks += 1
                    mark, mark_time = self.alive(step).position, self._clock()
                    continue
                lx, ly = stick_toward(player.position, player.control_yaw, goal)
                self._pad.set_pad({"lx": str(lx), "ly": str(ly)})
                self._sleep(POLL_SECONDS)
        finally:
            self._pad.release()
        self._record(step, begin, goal=goal, unblocks=unblocks,
                     seconds=round(self._clock() - start, 1))

    def wait(self, seconds: float) -> None:
        self._sleep(seconds)

    def holding(self, buttons: str, seconds: float) -> Callable[[], None]:
        """An unblock action that holds buttons for seconds."""

        def hold() -> None:
            self._pad.set_pad({"buttons": buttons})
            self._sleep(seconds)
            self._pad.release()

        return hold

    def fire(self, step: str, seconds: float, attempts: int,
             before_each: Callable[[], None] | None = None) -> int:
        """Aim and fire until the weapon counts rounds; returns the change in its count.

        A tutorial prompt can hold the trigger until its button is pressed
        under it, so each attempt runs before_each first. Refuses when no
        attempt fired.
        """

        begin = self.alive(step)
        if begin.magazine_rounds_fired is None:
            raise RouteFailure(f"{step}: the player holds no weapon")
        for attempt in range(1, attempts + 1):
            if before_each is not None:
                before_each()
            before = self.alive(step)
            self._pad.set_pad({"lt": "255"})
            self._sleep(0.8)
            self._pad.set_pad({"lt": "255", "rt": "255"})
            self._sleep(seconds)
            self._pad.release()
            end = self.alive(step)
            # A reload during the burst restarts the magazine count; either change is a shot.
            if end.magazine_rounds_fired != before.magazine_rounds_fired:
                self._record(step, begin, attempts=attempt,
                             rounds_before=before.magazine_rounds_fired,
                             rounds_after=end.magazine_rounds_fired)
                return end.magazine_rounds_fired - before.magazine_rounds_fired
        raise RouteFailure(f"{step}: the weapon counted no rounds in {attempts} attempts")


# Positions measured on the retail image's sp_prison_p (docs/re-frontier.md).
PATH_TO_DOOR = ((-922.0, 1124.0), (-1002.0, 1420.0), (-1123.0, 1936.0))
JAMMED_DOOR = (-1252.0, 2325.0)
YARD_PATH = (
    (-1253.0, 2637.0),
    (-1669.0, 2727.0),
    (-1752.0, 3104.0),
    (-1377.0, 3350.0),
    (-1034.0, 3530.0),
)
YARD_COVER = (-1034.0, 3640.0)


def play_to_first_firefight(route: Route) -> None:
    look = route.holding("Y", TUTORIAL_HOLD_SECONDS)
    route.hold("step out of cover", {"ly": str(-STICK_LIMIT)}, 1.0)
    for index, waypoint in enumerate(PATH_TO_DOOR):
        route.walk(f"cross to the door, waypoint {index + 1}", waypoint, radius=50.0,
                   timeout=30.0, unblock=look)
    route.walk("reach the jammed door", JAMMED_DOOR, radius=40.0, timeout=30.0, unblock=look)

    def open_door() -> None:
        # The kick waits for the objectives prompt, which follows about 20 s of
        # scripted radio dialogue after arrival, and for LB held under it. The
        # objectives display that LB opens swallows an X pressed as it closes,
        # so the kick waits for it to close first.
        route.holding("LB", TUTORIAL_HOLD_SECONDS)()
        route.wait(OBJECTIVES_CLOSE_SECONDS)
        route.holding("X", 0.3)()
        route.wait(3.0)

    route.walk("kick the jammed door open", YARD_PATH[0], radius=60.0, timeout=60.0,
               unblock=open_door, max_unblocks=6)
    for index, waypoint in enumerate(YARD_PATH[1:]):
        route.walk(f"cross to the yard, waypoint {index + 2}", waypoint, radius=60.0, timeout=25.0)
    route.walk("reach the yard cover", YARD_COVER, radius=60.0, timeout=15.0)
    # The cover prompt appears after arrival and holds the trigger until A is
    # pressed under it.
    take_cover = route.holding("A", 0.3)

    def take_cover_and_settle() -> None:
        take_cover()
        route.wait(1.5)

    route.fire("take cover with A and fire", seconds=2.0, attempts=6,
               before_each=take_cover_and_settle)


def wait_for_handover(control: ProductControl, run: subprocess.Popen[bytes]) -> None:
    """Wait until the walk has given up the pad and the player exists."""

    deadline = time.monotonic() + HANDOVER_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if run.poll() is not None:
            raise RouteFailure(f"the offscreen run ended with status {run.returncode} before the walk finished")
        try:
            if control.status()["input"]["source"] != "script":
                control.player()
                return
        except ControlError:
            pass
        time.sleep(2.0)
    raise RouteFailure(f"the walk did not hand over the pad within {HANDOVER_TIMEOUT_SECONDS} s")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=32126, help="the run's loopback control port")
    parser.add_argument("--iso", help="disc image or 7z archive (default: as ./run.sh)")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    report_root = REPO_ROOT / REPORT_ROOT
    report_root.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(REPO_ROOT / "tools/run_offscreen.py"), "--walk", "gameplay",
               "--seconds", str(RUN_SECONDS), "--control-port", str(arguments.port)]
    if arguments.iso:
        command += ["--iso", arguments.iso]
    control = ProductControl(arguments.port)
    route = Route(control)
    outcome: dict[str, object] = {"passed": False}
    with (report_root / "run.out").open("wb") as run_output:
        run = subprocess.Popen(command, cwd=REPO_ROOT, stdout=run_output, stderr=subprocess.STDOUT)
        try:
            wait_for_handover(control, run)
            play_to_first_firefight(route)
            outcome["passed"] = True
        except (RouteFailure, ControlError) as error:
            outcome["failure"] = str(error)
        try:
            control.frame().save(report_root / "final.png")
        except ControlError as error:
            outcome["final_frame"] = str(error)
        outcome["steps"] = route.steps
        (report_root / "report.json").write_text(json.dumps(outcome, indent=2) + "\n")
        run.wait()
    verdict = "reached the first firefight" if outcome["passed"] else f"FAILED: {outcome['failure']}"
    print(f"combat_route: {verdict}; report {report_root / 'report.json'}")
    return 0 if outcome["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
