#!/usr/bin/env python3
"""Play Gears 1 headless from a new campaign through Act 1's first firefight.

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
hostile the probe lists and follows Dom until the title saves its next
checkpoint (read from the run's storage by ``tools/gears1_checkpoint.py``).
Each tutorial holds
Marcus in place until its button is held. The report in ``scratch/combat_route/``
records where every step began and ended. The route fails, naming the step,
when a step does not reach its goal; it passes only when the player reached
cover in the yard, the weapon fired rounds there, every hostile of the first
firefight died, Marcus reached Dom afterwards and then a new checkpoint,
the world's game time kept
to wall time throughout (the product presents up to 120 times a second, twice
the console's rate), and the offscreen run's own
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
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.gears1_checkpoint import Checkpoint, read_checkpoint
from tools.navigation import NavigationError, NavigationGraph
from tools.product_control import ControlError, ProductControl
from tools.run_offscreen import STORAGE_ROOT

REPORT_ROOT = Path("scratch/combat_route")
STICK_LIMIT = 32767
YAW_UNITS_PER_TURN = 65536
POLL_SECONDS = 0.2
# A goal that moved the player less than this in STALL_SECONDS is blocked.
STALL_DISTANCE = 25.0
STALL_SECONDS = 2.5
# The walk takes about 300 s, the route to the yard about 120 s, and each of
# up to FIREFIGHT_ATTEMPTS firefights up to FIREFIGHT_SECONDS plus about 60 s
# to reload and return; the rest is margin. The route stops the run when it ends.
FIREFIGHT_SECONDS = 240.0
# The drones reach the yard some seconds after the player takes cover.
HOSTILE_ARRIVAL_SECONDS = 30.0
RUN_SECONDS = 1800
HANDOVER_TIMEOUT_SECONDS = 600
# Game time must keep to wall time while the product presents up to 120 times
# a second; the bound covers the control channel's sampling latency.
GAME_TIME_RATE_TOLERANCE = 0.03
TUTORIAL_HOLD_SECONDS = 2.5
OBJECTIVES_CLOSE_SECONDS = 1.5
DOOR_STEP_BACK_SECONDS = 0.3
# A late prompt can need a second hold; a third stall is a real obstacle.
MAX_UNBLOCKS = 2
# The walk's fixed timing leaves Marcus within this of WALK_END when it runs to time.
WALK_END_RADIUS = 120.0
# Aiming: the right stick turns the view; below about AIM_STICK_FLOOR it does
# nothing. Errors are engine angle units; AIM_HEIGHT raises the aim from a
# pawn's origin toward its chest.
AIM_STICK_FLOOR = 11000
AIM_GAIN = 14
AIM_TOLERANCE = 120
AIM_HEIGHT = 30.0
AIM_SECONDS = 2.0
BURST_SECONDS = 0.5
# Back in cover after a burst, long enough for hits to register.
COVER_SECONDS = 1.2
# Below this health the player stays in cover until it regenerates; one
# exposed burst has cost the player half of its 301 (docs/re-frontier.md).
RECOVER_HEALTH = 280
# Bursts at one target that leave its health unchanged before trying another.
BURSTS_PER_TARGET = 4
# A reload (RB) and a weapon switch (d-pad) take this long before the next
# burst; the slots are tried in this order for a weapon with ammunition.
RELOAD_SECONDS = 2.5
SWITCH_SECONDS = 1.0
WEAPON_SLOTS = ("RIGHT", "UP", "LEFT", "DOWN")
AIM_POLL_SECONDS = 0.04
RESPAWN_POLL_SECONDS = 3.0
RESPAWN_SETTLE_SECONDS = 3.0
RESPAWN_SECONDS = 120.0
# Deaths the first firefight may cost before the route gives up; each is
# followed by a reload of the yard checkpoint, as a player would.
FIREFIGHT_ATTEMPTS = 4
# A navigation point counts as reached this close, as the engine's own walkers
# accept; the points are about 150 to 200 units apart.
NAV_POINT_RADIUS = 60.0
# Points further above or below the player than this are on another floor.
NAV_HEIGHT = 150.0
NAV_POINT_SECONDS = 15.0
# The graph's path kinds the route can cover (tools/navigation.py).
TRAVEL_PATH_KINDS = ("walk", "mantle")
# Buttons held, each for seconds, with the stick toward the far side: the first
# A takes cover against the low wall, the second vaults it. Measured live at
# the yard's first cover (-1281, 3640), crossing to (-1284, 3792).
MANTLE_SEQUENCE = (("A", 0.3), (None, 0.8), ("A", 0.3), (None, 1.5))
# After the fight Dom walks on ahead; the route joins him once he has stood
# within SQUAD_SETTLE_DISTANCE for SQUAD_SETTLE_SECONDS.
SQUAD_SETTLE_DISTANCE = 50.0
SQUAD_SETTLE_SECONDS = 3.0
SQUAD_WAIT_SECONDS = 60.0
SQUAD_RADIUS = 250.0
SQUAD_POLL_SECONDS = 1.0
# Pulling the stick back this long leaves cover: a walk along a cover wall
# stops at its end, as it did from the yard cover.
LEAVE_COVER_SECONDS = 0.8
# From Dom, fighting and following him reached the next checkpoint in 90 s live.
ADVANCE_SECONDS = 300.0


class RouteFailure(RuntimeError):
    """A step did not reach its goal."""


@dataclass(frozen=True)
class Pawn:
    """One pawn of the world as the probe lists it."""

    id: int
    location: tuple[float, float, float]
    health: int
    team: int
    is_player: bool

    @staticmethod
    def from_json(reading: dict[str, object]) -> Pawn:
        x, y, z = (float(axis) for axis in reading["location"])
        return Pawn(int(reading["id"]), (x, y, z), int(reading["health"]), int(reading["team"]),
                    bool(reading["is_player"]))


@dataclass(frozen=True)
class Weapon:
    """The held weapon as the probe reads it."""

    id: int
    magazine_size: int
    rounds_fired: int
    spare_rounds: int

    @property
    def loaded(self) -> int:
        return max(0, self.magazine_size - self.rounds_fired)

    @staticmethod
    def from_json(reading: dict[str, object] | None) -> Weapon | None:
        if reading is None:
            return None
        return Weapon(int(reading["id"]), int(reading["magazine_size"]),
                      int(reading["magazine_rounds_fired"]), int(reading["spare_rounds"]))


@dataclass(frozen=True)
class Player:
    """One reading of the local player; position is None while the player is dead."""

    position: tuple[float, float] | None
    control_yaw: int
    weapon: Weapon | None
    world_seconds: float = 0.0
    control_pitch: int = 0
    height: float = 0.0
    health: int = 0
    team: int = 0
    pawns: tuple[Pawn, ...] = ()
    # The view's origin, over the pawn's right shoulder; aim is taken from here.
    camera: tuple[float, float, float] = (0.0, 0.0, 0.0)

    @staticmethod
    def from_json(reading: dict[str, object]) -> Player:
        pawn = reading["pawn"]
        world_seconds = float(reading["world_seconds"])
        pitch = int(reading["control_pitch"])
        pawns = tuple(Pawn.from_json(listed) for listed in reading["pawns"])
        cx, cy, cz = (float(axis) for axis in reading["camera_location"])
        if pawn is None:
            return Player(None, int(reading["control_yaw"]), None, world_seconds, pitch,
                          pawns=pawns, camera=(cx, cy, cz))
        location = pawn["location"]
        return Player(
            (float(location[0]), float(location[1])),
            int(reading["control_yaw"]),
            Weapon.from_json(pawn["weapon"]),
            world_seconds,
            pitch,
            float(location[2]),
            int(pawn["health"]),
            int(pawn["team"]),
            pawns,
            (cx, cy, cz),
        )

    @property
    def magazine_rounds_fired(self) -> int | None:
        return None if self.weapon is None else self.weapon.rounds_fired

    def squad(self) -> list[Pawn]:
        """Living pawns of the player's team other than the player's own."""

        return [pawn for pawn in self.pawns
                if pawn.team == self.team and pawn.health > 0 and not pawn.is_player]

    def hostiles(self) -> list[Pawn]:
        """Living pawns of another team than the player's."""

        return [pawn for pawn in self.pawns
                if pawn.team != self.team and pawn.health > 0 and not pawn.is_player]


class Pad(Protocol):
    """The part of the control channel the route drives."""

    def player(self) -> dict[str, object]: ...

    def navigation(self) -> dict[str, object]: ...

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


def wrap_angle(units: int) -> int:
    """An engine angle difference reduced to (-half turn, half turn]."""

    half = YAW_UNITS_PER_TURN // 2
    return (units + half) % YAW_UNITS_PER_TURN - half


def aim_error(player: Player, target: tuple[float, float, float]) -> tuple[int, int]:
    """Yaw and pitch, in engine units, that turn the view onto a point above target.

    The crosshair's ray leaves the camera, not the pawn; aiming from the pawn
    misses by the shoulder offset, most at close range.
    """

    dx = target[0] - player.camera[0]
    dy = target[1] - player.camera[1]
    dz = target[2] + AIM_HEIGHT - player.camera[2]
    units = YAW_UNITS_PER_TURN / (2.0 * math.pi)
    yaw = round(math.atan2(dy, dx) * units)
    pitch = round(math.atan2(dz, math.hypot(dx, dy)) * units)
    return wrap_angle(yaw - player.control_yaw), wrap_angle(pitch - player.control_pitch)


def aim_stick(error: int) -> int:
    """Right-stick deflection that turns the view to close an angle error.

    The engine ignores deflections below about AIM_STICK_FLOOR, so a smaller
    proportional command would leave the error standing.
    """

    if abs(error) <= AIM_TOLERANCE // 2:
        return 0
    magnitude = min(STICK_LIMIT, max(AIM_STICK_FLOOR, abs(error) * AIM_GAIN))
    return int(math.copysign(magnitude, error))


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

    def clock(self) -> tuple[float, float]:
        """Wall seconds and the world's game seconds, read together."""

        return self._clock(), self.player().world_seconds

    def require_real_time(self, start: tuple[float, float]) -> float:
        """Game seconds per wall second since start; refuses a simulation off real time."""

        wall, world = self.clock()
        rate = (world - start[1]) / (wall - start[0])
        self.steps.append({"step": "game time kept to wall time", "rate": round(rate, 4),
                           "wall_seconds": round(wall - start[0], 1)})
        if abs(rate - 1.0) > GAME_TIME_RATE_TOLERANCE:
            raise RouteFailure(f"game time ran at {rate:.3f} game seconds per wall second")
        return rate

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

    def travel(self, step: str, goal: tuple[float, float, float], radius: float,
               unblock: Callable[[], None] | None = None) -> None:
        """Walk to goal along the shortest path of the level's navigation graph.

        The walk starts at the point nearest the player and ends at the point
        nearest goal, then walks the last stretch straight, stopping as soon as
        the player is within radius of goal. It follows walk
        paths on foot and mantles over low cover where the graph does. Refuses,
        naming the point, as walk does, or when the graph has no way there.
        """

        begin = self.alive(step)
        try:
            graph = NavigationGraph.from_json(self._pad.navigation())
            here = (*begin.position, begin.height)
            hops = graph.shortest_path(graph.nearest(here, NAV_HEIGHT).id,
                                       graph.nearest(goal, NAV_HEIGHT).id, TRAVEL_PATH_KINDS)
        except (ControlError, NavigationError) as error:
            raise RouteFailure(f"{step}: {error}") from error
        # The nearest point may lie behind the player; skip it when the next
        # one is already closer and is reached on foot.
        if len(hops) > 1 and hops[1].kind == "walk" and (
                distance(begin.position, hops[1].point.location[:2])
                < distance(hops[0].point.location[:2], hops[1].point.location[:2])):
            hops = hops[1:]
        for index, hop in enumerate(hops):
            # A goal such as a squad mate may stand on the last points.
            if distance(self.alive(step).position, goal[:2]) <= radius:
                break
            name = f"{step}: point {index + 1} of {len(hops)}"
            target = hop.point.location[:2]
            if hop.kind == "mantle":
                self.mantle(f"{name}, mantling", target)
            self.walk(name, target, radius=NAV_POINT_RADIUS, timeout=NAV_POINT_SECONDS,
                      unblock=unblock)
        self.walk(f"{step}: last stretch", goal[:2], radius=radius, timeout=NAV_POINT_SECONDS,
                  unblock=unblock)
        self._record(step, begin, goal=goal, points=len(hops),
                     mantles=sum(hop.kind == "mantle" for hop in hops))

    def mantle(self, step: str, goal: tuple[float, float]) -> None:
        """Cross low cover toward goal: A takes cover, and A again vaults it."""

        begin = self.alive(step)
        try:
            for buttons, seconds in MANTLE_SEQUENCE:
                until = self._clock() + seconds
                while self._clock() < until:
                    player = self.alive(step)
                    lx, ly = stick_toward(player.position, player.control_yaw, goal)
                    fields = {"lx": str(lx), "ly": str(ly)}
                    if buttons:
                        fields["buttons"] = buttons
                    self._pad.set_pad(fields)
                    self._sleep(POLL_SECONDS / 2)
        finally:
            self._pad.release()
        self._record(step, begin, goal=goal)

    def wait(self, seconds: float) -> None:
        self._sleep(seconds)

    def pressing(self, fields: dict[str, str], seconds: float) -> Callable[[], None]:
        """An unblock action that holds pad fields for seconds."""

        def hold() -> None:
            self._pad.set_pad(fields)
            self._sleep(seconds)
            self._pad.release()

        return hold

    def holding(self, buttons: str, seconds: float) -> Callable[[], None]:
        """An unblock action that holds buttons for seconds."""

        return self.pressing({"buttons": buttons}, seconds)

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


    def respawn(self, step: str, timeout: float) -> None:
        """Load the last checkpoint from the death screen and wait for the new pawn.

        The death screen offers Load Last Checkpoint first, so A selects it;
        A is pressed only while the player has no pawn. Refuses when no pawn
        appears within timeout.
        """

        start = self._clock()
        while self.player().position is None:
            if self._clock() - start > timeout:
                raise RouteFailure(f"{step}: no pawn within {timeout:.0f} s of the death screen")
            self.holding("A", 0.3)()
            self._sleep(RESPAWN_POLL_SECONDS)
        # The checkpoint's opening camera holds the pawn briefly.
        self._sleep(RESPAWN_SETTLE_SECONDS)
        player = self.player()
        self.steps.append({"step": step, "to": player.position,
                           "seconds": round(self._clock() - start, 1)})

    def clear_firefight(self, step: str, timeout: float, arrival: float) -> int:
        """Fight from cover until no living hostile is listed; returns the hostiles fought.

        Waits up to arrival seconds for hostiles to appear. Each round turns
        the view onto the nearest hostile from cover, raises the weapon with
        LT only to correct the aim and fire a burst, and drops back into
        cover; below RECOVER_HEALTH it waits in cover instead. An empty
        magazine is reloaded and an empty weapon swapped. A target that
        takes no damage from BURSTS_PER_TARGET bursts yields to the others.
        The step's record lists every burst, also when the step fails.
        Refuses when the player dies, no hostile ever appears, or hostiles
        remain at the timeout.
        """

        begin = self.alive(step)
        start = self._clock()
        while not begin.hostiles():
            if self._clock() - start > arrival:
                raise RouteFailure(f"{step}: no hostile appeared within {arrival:.0f} s")
            self._sleep(POLL_SECONDS)
            begin = self.alive(step)
        seen = {pawn.id for pawn in begin.hostiles()}
        misses: dict[int, int] = {}
        bursts: list[dict[str, int | None]] = []
        try:
            while True:
                player = self.alive(step)
                hostiles = player.hostiles()
                seen.update(pawn.id for pawn in hostiles)
                if not hostiles:
                    break
                if self._clock() - start > timeout:
                    raise RouteFailure(
                        f"{step}: {len(hostiles)} hostile(s) still standing after {timeout:.0f} s"
                    )
                if player.health < RECOVER_HEALTH:
                    self._pad.release()
                    self._sleep(COVER_SECONDS)
                    continue
                target = min(hostiles, key=lambda pawn: (
                    misses.get(pawn.id, 0) // BURSTS_PER_TARGET,
                    distance(player.position, pawn.location[:2])))
                after, fired = self._burst(step, target)
                bursts.append({"target": target.id, "health": target.health,
                               "after": None if after is None else after.health,
                               "own_health": player.health, "fired": fired})
                if after is not None and after.health == target.health:
                    misses[target.id] = misses.get(target.id, 0) + 1
        except RouteFailure as failure:
            self._record(step, begin, failure=str(failure), hostiles=len(seen), bursts=bursts)
            raise
        finally:
            self._pad.release()
        self._record(step, begin, hostiles=len(seen), bursts=bursts,
                     seconds=round(self._clock() - start, 1))
        return len(seen)

    def _burst(self, step: str, target: Pawn) -> tuple[Pawn | None, bool]:
        """Aim at target from cover, fire one burst, and return to cover.

        Returns the target as listed afterwards (None once it has left the
        list) and whether the weapon fired.
        """

        self._ready_weapon(step)
        self._aim(step, target, {})
        self._aim(step, target, {"lt": "255"})
        before = self.alive(step).magazine_rounds_fired
        self._pad.set_pad({"lt": "255", "rt": "255"})
        self._sleep(BURST_SECONDS)
        self._pad.release()
        player = self.alive(step)
        fired = player.magazine_rounds_fired != before
        self._sleep(COVER_SECONDS)
        after = next((pawn for pawn in self.alive(step).pawns if pawn.id == target.id), None)
        return after, fired

    def _ready_weapon(self, step: str) -> None:
        """Leave a loaded weapon in hand, as a player would before firing.

        An empty magazine with spare rounds is reloaded (RB); a weapon with
        none is swapped for the first d-pad slot holding one that has.
        Refuses when no slot does.
        """

        weapon = self.alive(step).weapon
        if weapon is None:
            raise RouteFailure(f"{step}: the player holds no weapon")
        if weapon.loaded > 0:
            return
        if weapon.spare_rounds > 0:
            self.holding("RB", 0.3)()
            self._sleep(RELOAD_SECONDS)
            return
        for slot in WEAPON_SLOTS:
            self.holding(slot, 0.3)()
            self._sleep(SWITCH_SECONDS)
            weapon = self.alive(step).weapon
            if weapon is not None and weapon.loaded + weapon.spare_rounds > 0:
                self.steps.append({"step": f"{step}: switch to the weapon on {slot}",
                                   "loaded": weapon.loaded, "spare": weapon.spare_rounds})
                if weapon.loaded == 0:
                    self.holding("RB", 0.3)()
                    self._sleep(RELOAD_SECONDS)
                return
        raise RouteFailure(f"{step}: every weapon is out of ammunition")

    def _aim(self, step: str, target: Pawn, held: dict[str, str]) -> None:
        """Turn the view onto target with held pad fields, for at most AIM_SECONDS."""

        start = self._clock()
        while self._clock() - start < AIM_SECONDS:
            yaw_error, pitch_error = aim_error(self.alive(step), target.location)
            if abs(yaw_error) <= AIM_TOLERANCE and abs(pitch_error) <= AIM_TOLERANCE:
                return
            self._pad.set_pad({**held, "rx": str(aim_stick(yaw_error)),
                               "ry": str(aim_stick(pitch_error))})
            self._sleep(AIM_POLL_SECONDS)


# Positions measured on the retail image's sp_prison_p (docs/re-frontier.md).
# Where the walk usually leaves Marcus: in cover past the path choice.
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
    # trigger picks a path; LT is combat.
    route.walk("take the combat path", WALK_END, radius=WALK_END_RADIUS, timeout=40.0,
               unblock=route.pressing({"lt": "255"}, 0.5))
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
    """Wait for the squad mate to stop walking ahead, then travel to him."""

    waited = 0.0
    mark: tuple[float, float, float] | None = None
    settled = 0.0
    while True:
        squad = route.alive(step).squad()
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
    route.travel(step, here, radius=SQUAD_RADIUS,
                 unblock=route.pressing({"ly": str(-STICK_LIMIT)}, LEAVE_COVER_SECONDS))


def advance_to_next_checkpoint(route: Route,
                               saved: Callable[[], Checkpoint | None]) -> Checkpoint:
    """Fight every hostile and follow Dom until the title saves a new checkpoint.

    saved reads the checkpoint the title last saved. Refuses when the player
    dies, a fight or a walk fails, or no new checkpoint is saved within
    ADVANCE_SECONDS.
    """

    step = "advance to the next checkpoint"
    begin = route.alive(step)
    start = saved()
    started = route.clock()[0]
    fights = follows = 0
    while True:
        checkpoint = saved()
        if checkpoint is not None and checkpoint != start:
            route.steps.append({"step": step, "from": begin.position,
                                "to": route.player().position, "checkpoint": checkpoint.name,
                                "level": checkpoint.level, "fights": fights, "follows": follows})
            return checkpoint
        if route.clock()[0] - started > ADVANCE_SECONDS:
            last = "none" if start is None else start.name
            raise RouteFailure(f"{step}: the title saved no checkpoint past {last} "
                               f"within {ADVANCE_SECONDS:.0f} s")
        if route.alive(step).hostiles():
            fights += 1
            route.clear_firefight(f"{step}: firefight {fights}", FIREFIGHT_SECONDS, 0.0)
        else:
            follows += 1
            join_squad(route, f"{step}: follow Dom {follows}")


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
        "--verify-audio-mix",
        action="store_true",
        help="compare the native audio mix with the guest's body on every call during the route",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    report_root = REPO_ROOT / REPORT_ROOT
    report_root.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(REPO_ROOT / "tools/run_offscreen.py"), "--walk", "gameplay",
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
            start = route.clock()
            play_to_first_firefight(route)
            outcome["deaths"] = clear_first_firefight(route)
            join_squad(route)
            checkpoint = advance_to_next_checkpoint(
                route, lambda: read_checkpoint(REPO_ROOT / STORAGE_ROOT))
            outcome["checkpoint"] = f"{checkpoint.level}.{checkpoint.name}"
            route.require_real_time(start)
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
    verdict = (f"cleared the first firefight and reached {outcome['checkpoint']}"
               if outcome["passed"] else f"FAILED: {outcome['failure']}")
    print(f"combat_route: {verdict}; report {report_root / 'report.json'}")
    return 0 if outcome["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
