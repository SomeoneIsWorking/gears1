"""The Gears 1 world as the combat route reads it, and the geometry it steers by.

The control channel's ``/api/player`` reading becomes a Player with its pawns
and held weapon; the functions turn positions and engine angles into stick
deflections, aim errors, and the level's cover slot to fight from. None of it
touches the product, so tests exercise it directly.
"""

from __future__ import annotations

import math
from collections.abc import Collection, Sequence
from dataclasses import dataclass

from tools.navigation import NavigationGraph, Point

STICK_LIMIT = 32767
YAW_UNITS_PER_TURN = 65536
# Aiming: the right stick turns the view; below about AIM_STICK_FLOOR it does
# nothing. Errors are engine angle units; AIM_HEIGHT raises the aim from a
# pawn's origin toward its chest.
AIM_STICK_FLOOR = 11000
AIM_GAIN = 14
AIM_TOLERANCE = 120
AIM_HEIGHT = 30.0
# Points further above or below the player than this are on another floor.
NAV_HEIGHT = 150.0
# The graph's path kinds the route can cover (tools/navigation.py).
TRAVEL_PATH_KINDS = ("walk", "mantle")
# Cover slots face the way their yaw points: of those within
# COVER_SEARCH_RADIUS that stand at least COVER_HOSTILE_MARGIN from every
# hostile, the one whose facing covers the most hostiles (each within COVER_ARC
# of it) wins, then the nearest.
COVER_SEARCH_RADIUS = 800.0
COVER_HOSTILE_MARGIN = 450.0
COVER_ARC = YAW_UNITS_PER_TURN // 6


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
    """The held weapon as the probe reads it.

    rounds_fired rises by one for each round and falls when a reload refills
    the magazine; the probe cannot tell how many rounds remain, so a weapon's
    ammunition shows only in whether a burst moves the count.
    """

    id: int
    rounds_fired: int

    @staticmethod
    def from_json(reading: dict[str, object] | None) -> Weapon | None:
        if reading is None:
            return None
        return Weapon(int(reading["id"]), int(reading["rounds_fired"]))


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
    def rounds_fired(self) -> int | None:
        return None if self.weapon is None else self.weapon.rounds_fired

    def squad(self) -> list[Pawn]:
        """Living pawns of the player's team other than the player's own."""

        return [pawn for pawn in self.pawns
                if pawn.team == self.team and pawn.health > 0 and not pawn.is_player]

    def downed_squad(self) -> list[Pawn]:
        """Squad mates lying downed, waiting for a revive."""

        return [pawn for pawn in self.pawns
                if pawn.team == self.team and pawn.health == 0 and not pawn.is_player]

    def hostiles(self) -> list[Pawn]:
        """Living pawns of another team than the player's."""

        return [pawn for pawn in self.pawns
                if pawn.team != self.team and pawn.health > 0 and not pawn.is_player]


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


def bearing(origin: tuple[float, float], point: tuple[float, float]) -> int:
    """The yaw, in engine units, from origin toward point."""

    units = YAW_UNITS_PER_TURN / (2.0 * math.pi)
    return round(math.atan2(point[1] - origin[1], point[0] - origin[0]) * units)


def covered(slot: Point, hostile: Pawn) -> bool:
    """Whether the slot's cover faces the hostile, within COVER_ARC."""

    return abs(wrap_angle(bearing(slot.location[:2], hostile.location[:2]) - slot.yaw)) <= COVER_ARC


def choose_cover(graph: NavigationGraph, player: Player, hostiles: Sequence[Pawn],
                 avoid: Collection[int] = ()) -> Point | None:
    """The cover slot on the player's floor whose cover faces the most hostiles, then the nearest.

    Only slots within COVER_SEARCH_RADIUS, reachable from the player over
    TRAVEL_PATH_KINDS paths, COVER_HOSTILE_MARGIN from every hostile, facing
    at least one of them, and not in avoid count; None when there is none.
    """

    here = graph.nearest((*player.position, player.height), NAV_HEIGHT)
    reachable = graph.reachable(here.id, TRAVEL_PATH_KINDS)

    def usable(slot: Point) -> bool:
        ground = slot.location[:2]
        return (slot.id not in avoid
                and slot.id in reachable
                and abs(slot.location[2] - player.height) <= NAV_HEIGHT
                and distance(player.position, ground) <= COVER_SEARCH_RADIUS
                and all(distance(ground, hostile.location[:2]) >= COVER_HOSTILE_MARGIN
                        for hostile in hostiles))

    facing = [(count, slot) for slot in graph.of_kind("cover") if usable(slot)
              if (count := sum(covered(slot, hostile) for hostile in hostiles))]
    if not facing:
        return None
    return min(facing, key=lambda entry: (-entry[0],
                                          distance(player.position, entry[1].location[:2])))[1]
