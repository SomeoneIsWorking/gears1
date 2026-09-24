#!/usr/bin/env python3
"""Play Gears 1 headless from a new campaign through its first checkpoints.

The profile's gameplay walk is fixed-time input, so where it leaves Marcus
varies from run to run. This route continues from there with closed-loop
steering: it reads the local player through the control channel
(``/api/player``) and turns the left stick toward each goal until the player
stands there, so every run reaches the same places:

    uv run --locked python tools/combat_route.py

It starts ``tools/run_offscreen.py --walk gameplay`` with a control port, waits
for the walk to hand over the pad, and then takes the combat path when a slow
walk stopped at the path choice, steps out of the cover the walk ends in,
answers the points-of-interest tutorial with Y, walks to the jammed prison
door, answers the objectives tutorial with LB, kicks the door open, crosses the
cell room to the yard, takes cover, fires, and fights from that cover until
every hostile the probe lists is dead; a death reloads the yard checkpoint and
the fight resumes from the same cover, as a player would, up to a bound. When the
fight is over it waits for Dom to stop walking ahead and joins him along the
shortest path of the level's navigation graph (``/api/navigation``), which
goes round the walls a straight walk runs into. From there it fights every
hostile the probe lists from the level's nearest cover slot, revives Dom when he is downed, and follows him until
the title saves its next checkpoint (read from the run's storage by
``tools/gears1_checkpoint.py``); a death reloads the checkpoint.
``--checkpoints N`` plays on through N saves. ``--continue`` instead resumes
the profile's last saved checkpoint through Continue Campaign (the profile's
continue walk) and plays on from there, skipping the opening.
Each tutorial holds Marcus in place until its button is held, and a prompt that
blocks his input is dismissed with A when a burst, walk, or revive has no effect. The report in ``scratch/combat_route/``
records where every step began and ended. The route fails, naming the step,
when a step does not reach its goal; it passes only when, in a new campaign, the player reached
cover in the yard, the weapon fired rounds there, every hostile of the first firefight died, and
Marcus reached Dom afterwards; then each requested new checkpoint was saved,
the world's game time kept to wall time while the world ran (the product
presents up to 120 times a second, twice the console's rate), and the offscreen run's own
checks passed (with ``--verify-audio-mix``, that the native audio mix agreed
with the guest's body on every compared call).
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.combat_world import STICK_LIMIT
from tools.gears1_checkpoint import Checkpoint, read_checkpoint
from tools.product_control import ControlError, ProductControl
from tools.route_driver import STALL_SECONDS, Route, RouteFailure
from tools.run_offscreen import STORAGE_ROOT

REPORT_ROOT = Path("scratch/combat_route")
# The walk takes about 300 s, the route to the yard about 120 s, and each of
# up to FIREFIGHT_ATTEMPTS firefights up to FIREFIGHT_SECONDS plus about 60 s
# to reload and return; the rest is margin. The route stops the run when it ends.
FIREFIGHT_SECONDS = 240.0
# The drones reach the yard some seconds after the player takes cover.
HOSTILE_ARRIVAL_SECONDS = 30.0
RUN_SECONDS = 1800
HANDOVER_TIMEOUT_SECONDS = 600
TUTORIAL_HOLD_SECONDS = 2.5
OBJECTIVES_CLOSE_SECONDS = 1.5
DOOR_STEP_BACK_SECONDS = 0.3
# The path choice follows Dom's dialogue after the walk ends; a slow walk
# can end before it and wait there.
PATH_CHOICE_SECONDS = 40.0
# The walk's fixed timing leaves Marcus within this of WALK_END when it runs to time.
WALK_END_RADIUS = 120.0
RESPAWN_SECONDS = 120.0
# Deaths the first firefight may cost before the route gives up; each is
# followed by a reload of the yard checkpoint, as a player would.
FIREFIGHT_ATTEMPTS = 4
# After the fight Dom walks on ahead; the route joins him once he has stood
# within SQUAD_SETTLE_DISTANCE for SQUAD_SETTLE_SECONDS.
SQUAD_SETTLE_DISTANCE = 50.0
SQUAD_SETTLE_SECONDS = 3.0
SQUAD_WAIT_SECONDS = 60.0
SQUAD_RADIUS = 250.0
SQUAD_POLL_SECONDS = 1.0
# From Dom, fighting and following him reached the next checkpoint in 90 s live.
ADVANCE_SECONDS = 300.0
# Deaths one advance may cost; each reloads the checkpoint it started from.
ADVANCE_DEATHS = 4


WALK_END = (-760.0, 1190.0)
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
    # A slow walk can end before the path choice, which holds Marcus until a
    # trigger picks a path; LT is combat. The choice appears only after Dom's
    # dialogue, so LT is pressed at every stall until the step's timeout.
    route.walk("take the combat path", WALK_END, radius=WALK_END_RADIUS,
               timeout=PATH_CHOICE_SECONDS, unblock=route.pressing({"lt": "255"}, 0.5),
               max_unblocks=math.ceil(PATH_CHOICE_SECONDS / STALL_SECONDS))
    route.hold("step out of cover", {"ly": str(-STICK_LIMIT)}, 1.0)
    for index, waypoint in enumerate(PATH_TO_DOOR):
        route.walk(f"cross to the door, waypoint {index + 1}", waypoint, radius=50.0,
                   timeout=30.0, unblock=look)
    route.walk("reach the jammed door", JAMMED_DOOR, radius=40.0, timeout=30.0, unblock=look)

    def open_door() -> None:
        # The kick waits for the objectives prompt, which follows about 20 s of
        # scripted radio dialogue after arrival, and for LB held under it. The
        # objectives display that LB opens swallows an X pressed as it closes,
        # so the kick waits for it to close first. The walk leaves Marcus
        # pressed against the door; one live run whose kicks all failed so
        # opened it by hand after a step back.
        route.pressing({"ly": str(-STICK_LIMIT)}, DOOR_STEP_BACK_SECONDS)()
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


def take_yard_cover(route: Route) -> None:
    """From the yard checkpoint, walk to the first firefight's cover and take it."""

    for index, waypoint in enumerate(YARD_PATH):
        route.walk(f"return to the yard, waypoint {index + 1}", waypoint, radius=60.0,
                   timeout=30.0)
    route.walk("reach the yard cover again", YARD_COVER, radius=60.0, timeout=15.0)
    route.holding("A", 0.3)()
    route.wait(1.5)


def clear_first_firefight(route: Route) -> int:
    """Clear the yard's first firefight, reloading its checkpoint after each death.

    Returns the deaths it cost. Refuses on any other failure, and after
    FIREFIGHT_ATTEMPTS deaths.
    """

    attempt = 1
    while True:
        try:
            route.clear_firefight(f"clear the yard's first firefight, attempt {attempt}",
                                  FIREFIGHT_SECONDS, HOSTILE_ARRIVAL_SECONDS)
            return attempt - 1
        except RouteFailure:
            if route.player().position is not None or attempt == FIREFIGHT_ATTEMPTS:
                raise
        route.respawn(f"reload the yard checkpoint after death {attempt}", RESPAWN_SECONDS)
        take_yard_cover(route)
        attempt += 1


def join_squad(route: Route, step: str = "join Dom after the firefight") -> None:
    """Wait for the squad mate to stop walking ahead, then travel to him.

    A squad mate who is downed while Marcus waits or walks to him is revived.
    """

    waited = 0.0
    mark: tuple[float, float, float] | None = None
    settled = 0.0
    while True:
        player = route.alive(step)
        downed = player.downed_squad()
        if downed:
            route.revive(f"{step}: revive Dom", downed[0])
            mark, settled = None, 0.0
            continue
        squad = player.squad()
        if len(squad) != 1:
            raise RouteFailure(f"{step}: the squad lists {len(squad)} living mates, not Dom alone")
        here = squad[0].location
        if mark is not None and math.dist(here, mark) <= SQUAD_SETTLE_DISTANCE:
            settled += SQUAD_POLL_SECONDS
            if settled >= SQUAD_SETTLE_SECONDS:
                break
        else:
            mark, settled = here, 0.0
        if waited >= SQUAD_WAIT_SECONDS:
            raise RouteFailure(f"{step}: Dom was still walking after {SQUAD_WAIT_SECONDS:.0f} s")
        route.wait(SQUAD_POLL_SECONDS)
        waited += SQUAD_POLL_SECONDS
    # He may go down while Marcus walks to him; the walk then ends to revive him.
    route.travel(step, here, radius=SQUAD_RADIUS, unblock=route.unstick,
                 until=lambda player: bool(player.downed_squad()))
    downed = route.alive(step).downed_squad()
    if downed:
        route.revive(f"{step}: revive Dom", downed[0])


def wait_for_squad(route: Route, timeout: float) -> None:
    """Wait until the level lists a squad mate, standing or downed.

    A resumed checkpoint gives the player his pawn before the level spawns
    Dom. Refuses after timeout.
    """

    step = "wait for the squad"
    begin = route.alive(step)
    waited = 0.0
    while not (begin.squad() or begin.downed_squad()):
        if waited >= timeout:
            raise RouteFailure(f"{step}: no squad mate was listed within {timeout:.0f} s")
        route.wait(SQUAD_POLL_SECONDS)
        waited += SQUAD_POLL_SECONDS
        begin = route.alive(step)
    route.steps.append({"step": step, "seconds": waited})


def advance_to_next_checkpoint(route: Route,
                               saved: Callable[[], Checkpoint | None]) -> Checkpoint:
    """Play on until the title saves a new checkpoint.

    Each round fights every listed hostile from cover, revives a downed squad mate, or
    follows Dom, in that order. A
    death reloads the checkpoint the advance started from, up to
    ADVANCE_DEATHS times. saved reads the checkpoint the title last saved.
    Refuses when a step fails while the player lives, after too many deaths,
    or when no new checkpoint is saved within ADVANCE_SECONDS.
    """

    step = "advance to the next checkpoint"
    # The player may start dead, from a death at the end of the previous step.
    begin = route.player()
    start = saved()
    started = route.clock()[0]
    counts = {"fights": 0, "revives": 0, "follows": 0, "deaths": 0}
    while True:
        checkpoint = saved()
        if checkpoint is not None and checkpoint != start:
            route.steps.append({"step": step, "from": begin.position,
                                "to": route.player().position, "checkpoint": checkpoint.name,
                                "level": checkpoint.level, **counts})
            return checkpoint
        if route.clock()[0] - started > ADVANCE_SECONDS:
            last = "none" if start is None else start.name
            raise RouteFailure(f"{step}: the title saved no checkpoint past {last} "
                               f"within {ADVANCE_SECONDS:.0f} s")
        if route.player().position is None:
            counts["deaths"] += 1
            if counts["deaths"] > ADVANCE_DEATHS:
                raise RouteFailure(f"{step}: the player died {counts['deaths']} times")
            route.respawn(f"{step}: reload the checkpoint after death {counts['deaths']}",
                          RESPAWN_SECONDS)
            continue
        try:
            player = route.alive(step)
            if player.hostiles():
                counts["fights"] += 1
                route.clear_firefight(f"{step}: firefight {counts['fights']}",
                                      FIREFIGHT_SECONDS, 0.0, cover=True)
            elif player.downed_squad():
                counts["revives"] += 1
                route.revive(f"{step}: revive {counts['revives']}", player.downed_squad()[0])
            else:
                counts["follows"] += 1
                join_squad(route, f"{step}: follow Dom {counts['follows']}")
        except RouteFailure:
            # A death ends whatever step was under way; the loop reloads.
            if route.player().position is not None:
                raise


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
    parser.add_argument(
        "--hold-on-failure",
        action="store_true",
        help="after a failure, leave the run on its control port to be examined, until it is "
        "stopped through the port or reaches its time limit",
    )
    parser.add_argument("--checkpoints", type=int, default=1,
                        help="new checkpoints to reach before the route passes")
    parser.add_argument(
        "--continue",
        dest="resume",
        action="store_true",
        help="resume the last saved checkpoint instead of starting a new campaign; refuses when "
        "none is saved",
    )
    parser.add_argument(
        "--verify-audio-mix",
        action="store_true",
        help="compare the native audio mix with the guest's body on every call during the route",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    if arguments.checkpoints < 1:
        raise SystemExit("--checkpoints must be at least 1")
    storage = REPO_ROOT / STORAGE_ROOT
    resumed = read_checkpoint(storage) if arguments.resume else None
    if arguments.resume and resumed is None:
        raise SystemExit(f"--continue: no checkpoint is saved under {STORAGE_ROOT}")
    report_root = REPO_ROOT / REPORT_ROOT
    report_root.mkdir(parents=True, exist_ok=True)
    walk = "continue" if arguments.resume else "gameplay"
    command = [sys.executable, str(REPO_ROOT / "tools/run_offscreen.py"), "--walk", walk,
               "--seconds", str(RUN_SECONDS), "--control-port", str(arguments.port)]
    if arguments.iso:
        command += ["--iso", arguments.iso]
    if arguments.verify_audio_mix:
        command.append("--verify-audio-mix")
    control = ProductControl(arguments.port)
    route = Route(control)
    outcome: dict[str, object] = {"passed": False}
    with (report_root / "run.out").open("wb") as run_output:
        run = subprocess.Popen(command, cwd=REPO_ROOT, stdout=run_output, stderr=subprocess.STDOUT)
        try:
            wait_for_handover(control, run)
            route.start_timing()
            if resumed is None:
                play_to_first_firefight(route)
                outcome["deaths"] = clear_first_firefight(route)
                join_squad(route)
            else:
                outcome["resumed"] = f"{resumed.level}.{resumed.name}"
                wait_for_squad(route, SQUAD_WAIT_SECONDS)
            reached = []
            for _ in range(arguments.checkpoints):
                checkpoint = advance_to_next_checkpoint(route, lambda: read_checkpoint(storage))
                reached.append(f"{checkpoint.level}.{checkpoint.name}")
            outcome["checkpoints"] = reached
            route.require_real_time()
            outcome["passed"] = True
        except (RouteFailure, ControlError) as error:
            outcome["failure"] = str(error)
        try:
            control.frame().save(report_root / "final.png")
        except ControlError as error:
            outcome["final_frame"] = str(error)
        outcome["steps"] = route.steps
        # The run ends at the stop, or on its own at RUN_SECONDS, and fails
        # when its own checks do.
        if arguments.hold_on_failure and not outcome["passed"]:
            (report_root / "report.json").write_text(json.dumps(outcome, indent=2) + "\n")
            print(f"combat_route: FAILED: {outcome['failure']}; the run holds control port "
                  f"{arguments.port} until it is stopped", flush=True)
        else:
            try:
                control.stop()
            except ControlError as error:
                outcome["stop"] = str(error)
        outcome["run_status"] = run.wait()
        if outcome["passed"] and outcome["run_status"] != 0:
            outcome["passed"] = False
            outcome["failure"] = (
                f"the offscreen run failed its own checks with status {outcome['run_status']}; "
                "see scratch/offscreen/run.log"
            )
        (report_root / "report.json").write_text(json.dumps(outcome, indent=2) + "\n")
    verdict = (f"cleared the first firefight and reached {', '.join(outcome['checkpoints'])}"
               if outcome["passed"] else f"FAILED: {outcome['failure']}")
    print(f"combat_route: {verdict}; report {report_root / 'report.json'}")
    return 0 if outcome["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
