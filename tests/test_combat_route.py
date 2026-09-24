"""Closed-loop steering of the Gears 1 combat route against a simulated player."""

from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tools import combat_route as route_module
from tools.combat_route import Route, RouteFailure, stick_toward


class SimulatedPlayer:
    """Moves along the stick direction relative to its yaw, like the engine's walk input.

    The right stick turns the view only past the engine's dead zone. A burst
    fired with the view on a hostile's aim point costs it BURST_DAMAGE unless
    it is armoured; hostiles shoot the player while LT holds it out of cover.
    """

    BURST_DAMAGE = 90
    TURN_RATE = 4000.0  # engine units per second at full deflection
    DEAD_ZONE = 9000
    CAMERA_RIGHT = 40.0

    def __init__(self, position: tuple[float, float], yaw: int, speed: float = 60.0,
                 wall_x: float | None = None, wall_gap_y: float | None = None) -> None:
        self.position = position
        self.yaw = yaw
        self.pitch = 0
        self.speed = speed
        self.wall_x = wall_x
        self.wall_gap_y = wall_gap_y  # the wall stops short of this y, leaving a way round
        self.cover_slides = False  # held at the wall, any sideways stick runs along it at full speed
        self.graph: list[dict[str, object]] = []
        self.mates: list[dict[str, object]] = []  # squad mates, each walking at its velocity
        self.vaulting = False  # A pressed at the wall lets the next moves cross it
        self.stick = (0, 0)
        self.aim_stick = (0, 0)
        self.firing = False
        self.aiming = False
        self.alive = True
        self.health = 301
        self.now = 0.0
        self.world = 0.0
        self.time_scale = 1.0
        self.prompt = False  # a tutorial prompt: the pad does nothing but A, the world runs on
        self.hostiles: list[dict[str, object]] = []
        self.incoming = 0.0  # damage per second while exposed
        self.shots_on: list[int] = []
        # Weapons by d-pad slot; RB reloads the held one from its spares.
        self.weapons = {"RIGHT": {"id": 2, "size": 60, "fired": 0, "spare": 1000},
                        "DOWN": {"id": 3, "size": 12, "fired": 0, "spare": 36}}
        self.held = "RIGHT"
        self.burst_rounds = 0
        self.checkpoint: tuple[float, float] | None = None  # where A respawns a dead player
        self.incoming_after_respawn = 0.0

    def add_hostile(self, ident: int, location: tuple[float, float, float],
                    armoured: bool = False) -> None:
        self.hostiles.append({"id": ident, "location": location, "health": 250,
                              "armoured": armoured})

    # Pad protocol
    def player(self) -> dict[str, object]:
        pawns = [{"id": hostile["id"], "location": list(hostile["location"]),
                  "health": hostile["health"], "team": 1, "is_player": False}
                 for hostile in self.hostiles]
        pawns += [{"id": mate["id"], "location": list(mate["location"]),
                   "health": mate.get("health", 301), "team": 0, "is_player": False}
                  for mate in self.mates]
        reading = {"control_yaw": self.yaw % 65536, "control_pitch": self.pitch % 65536,
                   "camera_yaw": self.yaw % 65536, "camera_location": [*self.camera(), 0.0],
                   "world_seconds": self.world,
                   "pawn": None, "pawns": pawns}
        if self.alive:
            weapon = self.weapons[self.held]
            reading["pawn"] = {"location": [*self.position, 0.0], "health": self.health, "team": 0,
                               "weapon": {"id": weapon["id"], "rounds_fired": weapon["fired"]}}
            pawns.insert(0, {"id": 1, "location": [*self.position, 0.0], "health": self.health,
                             "team": 0, "is_player": True})
        return reading

    def navigation(self) -> dict[str, object]:
        return {"points": self.graph}

    def camera(self) -> tuple[float, float]:
        """The view's origin: CAMERA_RIGHT units right of the pawn, a quarter turn clockwise."""

        yaw = self.yaw * 2.0 * math.pi / route_module.YAW_UNITS_PER_TURN
        return (self.position[0] - self.CAMERA_RIGHT * math.sin(yaw),
                self.position[1] + self.CAMERA_RIGHT * math.cos(yaw))

    def set_pad(self, fields: dict[str, str]) -> None:
        self._apply(fields)

    def release(self) -> None:
        self._apply({})

    def _apply(self, fields: dict[str, str]) -> None:
        if self.prompt:
            self.prompt = fields.get("buttons") != "A"
            fields = {}
        if fields.get("buttons") == "X" and self.alive:
            for mate in self.mates:
                if (mate.get("health", 301) == 0
                        and math.dist(self.position, mate["location"][:2]) <= 100.0):
                    mate["health"] = 301
        if fields.get("buttons") == "A" and not self.alive and self.checkpoint is not None:
            self.alive, self.health, self.position = True, 301, self.checkpoint
            self.world = 0.0  # the load starts the level's clock again
            self.incoming = self.incoming_after_respawn
        if (fields.get("buttons") == "A" and self.alive and self.wall_x is not None
                and abs(self.position[0] - self.wall_x) < 80.0):
            self.vaulting = True
        elif fields.get("lx", "0") == "0" and fields.get("ly", "0") == "0":
            self.vaulting = False
        if fields.get("buttons") in self.weapons:
            self.held = fields["buttons"]
        if fields.get("buttons") == "RB":
            weapon = self.weapons[self.held]
            refill = min(weapon["fired"], weapon["spare"])
            weapon["fired"] -= refill
            weapon["spare"] -= refill
        if self.firing and fields.get("rt") != "255":
            self._resolve_burst()
        self.stick = (int(fields.get("lx", "0")), int(fields.get("ly", "0")))
        self.aim_stick = (int(fields.get("rx", "0")), int(fields.get("ry", "0")))
        self.firing = fields.get("rt") == "255"
        self.aiming = fields.get("lt") == "255"

    def _on_target(self) -> dict[str, object] | None:
        if not self.alive:
            return None
        for hostile in self.hostiles:
            if hostile["health"] <= 0:
                continue
            yaw_error, pitch_error = self._view_error(hostile["location"])
            if abs(yaw_error) <= 200 and abs(pitch_error) <= 200:
                return hostile
        return None

    def _view_error(self, target: tuple[float, float, float]) -> tuple[int, int]:
        """The simulation's own ray from the camera; independent of the route's aim_error."""

        x, y = self.camera()
        units = route_module.YAW_UNITS_PER_TURN / (2.0 * math.pi)
        yaw = math.atan2(target[1] - y, target[0] - x) * units
        pitch = math.atan2(target[2] + route_module.AIM_HEIGHT, math.hypot(target[0] - x, target[1] - y)) * units
        wrap = route_module.YAW_UNITS_PER_TURN
        return (round((yaw - self.yaw + wrap / 2) % wrap - wrap / 2),
                round((pitch - self.pitch + wrap / 2) % wrap - wrap / 2))

    def _resolve_burst(self) -> None:
        rounds, self.burst_rounds = self.burst_rounds, 0
        if rounds == 0:
            return
        hostile = self._on_target()
        if hostile is not None:
            self.shots_on.append(hostile["id"])
            if not hostile["armoured"]:
                hostile["health"] -= self.BURST_DAMAGE

    # Clock and sleep advance the simulation.
    def clock(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds
        self.world += seconds * self.time_scale
        for mate in self.mates:
            x, y, z = mate["location"]
            vx, vy = mate["velocity"]
            mate["location"] = (x + vx * seconds, y + vy * seconds, z)
        weapon = self.weapons[self.held]
        if self.firing:
            fired = min(weapon["size"] - weapon["fired"], round(20 * seconds))
            weapon["fired"] += fired
            self.burst_rounds += fired
        if self.aiming and any(hostile["health"] > 0 for hostile in self.hostiles):
            self.health -= round(self.incoming * seconds)
            if self.health <= 0:
                self.alive = False
        elif self.health < 301:
            self.health = min(301, self.health + round(150 * seconds))
        rx, ry = self.aim_stick
        if abs(rx) > self.DEAD_ZONE:
            self.yaw += round(rx / route_module.STICK_LIMIT * self.TURN_RATE * seconds)
        if abs(ry) > self.DEAD_ZONE:
            self.pitch += round(ry / route_module.STICK_LIMIT * self.TURN_RATE * seconds)
        lx, ly = (value / route_module.STICK_LIMIT for value in self.stick)
        yaw = self.yaw * 2.0 * math.pi / route_module.YAW_UNITS_PER_TURN
        # Forward is along yaw; right is a quarter turn toward +y in the left-handed world.
        dx = ly * math.cos(yaw) - lx * math.sin(yaw)
        dy = ly * math.sin(yaw) + lx * math.cos(yaw)
        x = self.position[0] + dx * self.speed * seconds * 10
        y = self.position[1] + dy * self.speed * seconds * 10
        blocked = not self.vaulting and (self.wall_gap_y is None
                                         or min(y, self.position[1]) < self.wall_gap_y)
        if self.wall_x is not None and blocked and (x > self.wall_x) != (self.position[0] > self.wall_x):
            x = self.position[0]
            if self.cover_slides and dy:
                y = self.position[1] + math.copysign(self.speed * seconds * 10, dy)
        self.position = (x, y)


def route_over(player: SimulatedPlayer) -> Route:
    return Route(player, clock=player.clock, sleep=player.sleep)


class StickTest(unittest.TestCase):
    def test_goal_ahead_is_full_forward(self) -> None:
        self.assertEqual(stick_toward((0.0, 0.0), 0, (100.0, 0.0)), (0, 32767))

    def test_goal_a_quarter_turn_clockwise_is_full_right(self) -> None:
        # Facing +x, +y is to the right in the left-handed world.
        self.assertEqual(stick_toward((0.0, 0.0), 0, (0.0, 100.0)), (32767, 0))

    def test_yaw_is_subtracted(self) -> None:
        # Facing +y (yaw a quarter turn), a goal at +y is straight ahead.
        self.assertEqual(stick_toward((0.0, 0.0), 16384, (0.0, 100.0)), (0, 32767))


class WalkTest(unittest.TestCase):
    def test_reaches_a_goal_from_any_facing(self) -> None:
        for yaw in (0, 16384, 32768, 49152, 70000):
            player = SimulatedPlayer((0.0, 0.0), yaw)
            route = route_over(player)
            route.walk("reach", (-500.0, 300.0), radius=40.0, timeout=20.0)
            self.assertLessEqual(math.dist(player.position, (-500.0, 300.0)), 40.0 + 60.0 * 2)
            self.assertEqual(route.steps[-1]["step"], "reach")

    def test_sliding_along_cover_is_blocked_although_the_player_moves(self) -> None:
        player = SimulatedPlayer((60.0, 0.0), 0, wall_x=100.0)
        player.cover_slides = True
        with self.assertRaisesRegex(RouteFailure, "reach: blocked"):
            route_over(player).walk("reach", (300.0, 69.0), radius=40.0, timeout=20.0)

    def test_a_wall_is_reported_as_blocked(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0, wall_x=100.0)
        with self.assertRaisesRegex(RouteFailure, "reach: blocked"):
            route_over(player).walk("reach", (500.0, 0.0), radius=40.0, timeout=20.0)
        self.assertEqual(player.stick, (0, 0))

    def test_a_dead_player_fails_the_step(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.alive = False
        with self.assertRaisesRegex(RouteFailure, "reach: the player died"):
            route_over(player).walk("reach", (500.0, 0.0), radius=40.0, timeout=20.0)

    def test_running_out_of_time_names_the_distance(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0, speed=5.0)
        with self.assertRaisesRegex(RouteFailure, r"reach: still \d+ from"):
            route_over(player).walk("reach", (5000.0, 0.0), radius=40.0, timeout=10.0)


# A wall at x=100 from y below 300; the graph goes round its end.
ROUND_THE_WALL = [
    {"id": 1, "location": [0.0, 0.0, 0.0], "paths": [[2, 400, "walk"]]},
    {"id": 2, "location": [20.0, 400.0, 0.0], "paths": [[3, 200, "walk"]]},
    {"id": 3, "location": [220.0, 400.0, 0.0], "paths": [[4, 400, "walk"], [2, 200, "walk"]]},
    {"id": 4, "location": [300.0, 0.0, 0.0], "paths": [[3, 400, "walk"]]},
]


class TravelTest(unittest.TestCase):
    def test_follows_the_graph_round_a_wall(self) -> None:
        # Behind the first point, which the walk therefore keeps.
        player = SimulatedPlayer((-30.0, -60.0), 0, wall_x=100.0, wall_gap_y=300.0)
        player.graph = ROUND_THE_WALL
        route = route_over(player)
        route.travel("round", (310.0, 10.0, 0.0), radius=60.0)
        self.assertLessEqual(math.dist(player.position, (310.0, 10.0)), 60.0 + 120.0)
        self.assertEqual(route.steps[-1]["points"], 4)
        self.assertEqual([step["step"] for step in route.steps[:2]],
                         ["round: point 1 of 4", "round: point 2 of 4"])

    def test_a_goal_short_of_an_unreachable_point_ends_the_travel(self) -> None:
        # The point nearest the goal lies behind a wall with no way round; the
        # goal itself is reached before the wall.
        player = SimulatedPlayer((-200.0, 0.0), 0, wall_x=100.0)
        player.graph = [
            {"id": 1, "location": [-200.0, 0.0, 0.0], "paths": [[2, 400, "walk"]]},
            {"id": 2, "location": [200.0, 0.0, 0.0], "paths": [[1, 400, "walk"]]},
        ]
        route = route_over(player)
        route.travel("short", (130.0, 0.0, 0.0), radius=90.0)
        self.assertLessEqual(math.dist(player.position, (130.0, 0.0)), 90.0)
        self.assertEqual(route.steps[-1]["step"], "short")

    def test_a_straight_walk_is_stopped_by_the_same_wall(self) -> None:
        player = SimulatedPlayer((-30.0, 10.0), 0, wall_x=100.0, wall_gap_y=300.0)
        with self.assertRaisesRegex(RouteFailure, "blocked"):
            route_over(player).walk("straight", (310.0, 10.0), radius=40.0, timeout=20.0)

    def test_skips_a_nearest_point_behind_the_player(self) -> None:
        player = SimulatedPlayer((15.0, 150.0), 0, wall_x=100.0, wall_gap_y=300.0)
        player.graph = ROUND_THE_WALL
        route = route_over(player)
        route.travel("round", (310.0, 10.0, 0.0), radius=60.0)
        self.assertEqual(route.steps[-1]["points"], 3)

    def test_stops_once_within_the_goal_radius(self) -> None:
        # The last point is where the goal stands, as a squad mate does.
        player = SimulatedPlayer((-30.0, -60.0), 0, wall_x=100.0, wall_gap_y=300.0)
        player.graph = ROUND_THE_WALL
        route = route_over(player)
        route.travel("round", (220.0, 400.0, 0.0), radius=250.0)
        self.assertNotIn("round: point 3 of 3", [step["step"] for step in route.steps])

    def test_no_way_there_fails(self) -> None:
        player = SimulatedPlayer((310.0, 10.0), 0)
        player.graph = ROUND_THE_WALL
        with self.assertRaisesRegex(RouteFailure, "back: point 0x1 is unreachable"):
            route_over(player).travel("back", (0.0, 0.0, 0.0), radius=40.0)

    def test_a_level_without_a_graph_fails(self) -> None:
        with self.assertRaisesRegex(RouteFailure, "go: the level lists no navigation points"):
            route_over(SimulatedPlayer((0.0, 0.0), 0)).travel("go", (1.0, 1.0, 0.0), 40.0)


# The same wall with no end; a mantle crosses it.
OVER_THE_WALL = [
    {"id": 1, "location": [0.0, 0.0, 0.0], "paths": [[2, 60, "walk"]]},
    {"id": 2, "location": [60.0, 0.0, 0.0], "paths": [[3, 80, "mantle"], [3, 5, "0x820DF980"]]},
    {"id": 3, "location": [140.0, 0.0, 0.0], "paths": [[4, 160, "walk"]]},
    {"id": 4, "location": [300.0, 0.0, 0.0], "paths": []},
]


class MantleTest(unittest.TestCase):
    def test_mantles_where_the_graph_does(self) -> None:
        player = SimulatedPlayer((-40.0, 0.0), 0, speed=20.0, wall_x=100.0)
        player.graph = OVER_THE_WALL
        route = route_over(player)
        route.travel("over", (300.0, 0.0, 0.0), radius=60.0)
        self.assertGreater(player.position[0], 200.0)
        self.assertEqual(route.steps[-1]["mantles"], 1)
        self.assertIn("over: point 3 of 4, mantling", [step["step"] for step in route.steps])

    def test_a_walk_into_the_wall_is_blocked(self) -> None:
        player = SimulatedPlayer((60.0, 0.0), 0, speed=20.0, wall_x=100.0)
        with self.assertRaisesRegex(RouteFailure, "blocked"):
            route_over(player).walk("into", (160.0, 0.0), radius=20.0, timeout=20.0)

    def test_unknown_path_kinds_are_not_followed(self) -> None:
        player = SimulatedPlayer((-40.0, 0.0), 0, speed=20.0, wall_x=100.0)
        player.graph = [{**point, "paths": [path for path in point["paths"] if path[2] != "mantle"]}
                        for point in OVER_THE_WALL]
        with self.assertRaisesRegex(RouteFailure, "unreachable"):
            route_over(player).travel("over", (300.0, 0.0, 0.0), radius=60.0)


class JoinSquadTest(unittest.TestCase):
    def _player(self, velocity: tuple[float, float]) -> SimulatedPlayer:
        player = SimulatedPlayer((-30.0, -60.0), 0, wall_x=100.0, wall_gap_y=300.0)
        player.graph = ROUND_THE_WALL
        player.mates.append({"id": 2, "location": (200.0, -100.0, 0.0), "velocity": velocity})
        return player

    def test_joins_a_mate_once_he_stops(self) -> None:
        player = self._player((0.0, 0.0))
        route = route_over(player)
        route_module.join_squad(route)
        self.assertLessEqual(math.dist(player.position, player.mates[0]["location"][:2]),
                             route_module.SQUAD_RADIUS + 120.0)
        self.assertEqual(route.steps[-1]["step"], "join Dom after the firefight")

    def test_a_mate_who_keeps_walking_fails(self) -> None:
        with self.assertRaisesRegex(RouteFailure, "Dom was still walking after 60 s"):
            route_module.join_squad(route_over(self._player((0.0, 100.0))))

    def test_no_mate_fails(self) -> None:
        with self.assertRaisesRegex(RouteFailure, "the squad lists 0 living mates"):
            route_module.join_squad(route_over(SimulatedPlayer((0.0, 0.0), 0)))


class AdvanceTest(unittest.TestCase):
    CHECKPOINT = route_module.Checkpoint("sp_prison_p", "SP_Prison_S04_Scripting", "WarCheckpoint_1")
    NEXT = route_module.Checkpoint("sp_prison_p", "SP_Prison_S05_Scripting", "WarCheckpoint_3")

    def _player(self) -> SimulatedPlayer:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.graph = [{"id": 1, "location": [0.0, 0.0, 0.0], "paths": []}]
        player.mates.append({"id": 2, "location": (100.0, 0.0, 0.0), "velocity": (0.0, 0.0)})
        return player

    def test_fights_then_follows_until_a_new_checkpoint(self) -> None:
        player = self._player()
        player.add_hostile(7, (1000.0, 300.0, 0.0))

        def saved() -> route_module.Checkpoint:
            # The title saves once the fight is over and the player has moved on.
            done = all(hostile["health"] <= 0 for hostile in player.hostiles)
            return self.NEXT if done and player.now > 60.0 else self.CHECKPOINT

        route = route_over(player)
        self.assertEqual(route_module.advance_to_next_checkpoint(route, saved), self.NEXT)
        record = route.steps[-1]
        self.assertEqual((record["checkpoint"], record["fights"]), ("WarCheckpoint_3", 1))
        self.assertGreater(record["follows"], 0)

    def test_no_new_checkpoint_fails(self) -> None:
        with self.assertRaisesRegex(RouteFailure, "saved no checkpoint past WarCheckpoint_1 within 300 s"):
            route_module.advance_to_next_checkpoint(route_over(self._player()),
                                                    lambda: self.CHECKPOINT)

    def test_a_downed_mate_is_revived_before_following_him(self) -> None:
        player = self._player()
        player.mates[0]["health"] = 0

        def saved() -> route_module.Checkpoint:
            return self.NEXT if player.mates[0]["health"] > 0 else self.CHECKPOINT

        route = route_over(player)
        self.assertEqual(route_module.advance_to_next_checkpoint(route, saved), self.NEXT)
        self.assertEqual(route.steps[-1]["revives"], 1)
        self.assertIn("advance to the next checkpoint: revive 1",
                      [step["step"] for step in route.steps])

    def test_a_mate_who_stays_down_fails(self) -> None:
        player = self._player()
        player.mates[0].update(health=0, location=(100.0, 0.0, 0.0))
        route = route_over(player)
        player.set_pad = lambda fields: SimulatedPlayer._apply(
            player, {key: value for key, value in fields.items() if value != "X"})
        with self.assertRaisesRegex(RouteFailure, "revive 1: the squad mate was still down "
                                                  "after 5 presses of X"):
            route_module.advance_to_next_checkpoint(route, lambda: self.CHECKPOINT)

    def test_the_revive_prompt_is_dismissed_when_x_does_nothing(self) -> None:
        player = self._player()
        # Beside him, so no walk's unblock dismisses the prompt first.
        player.mates[0].update(health=0, location=(50.0, 0.0, 0.0))
        player.prompt = True

        def saved() -> route_module.Checkpoint:
            return self.NEXT if player.mates[0]["health"] > 0 else self.CHECKPOINT

        route = route_over(player)
        self.assertEqual(route_module.advance_to_next_checkpoint(route, saved), self.NEXT)
        self.assertFalse(player.prompt)
        # The travel to him is recorded under the same name before the presses.
        revive = [step for step in route.steps
                  if step["step"] == "advance to the next checkpoint: revive 1"][-1]
        self.assertEqual(revive["presses"], 2)

    def test_a_mate_downed_during_the_walk_to_him_is_revived(self) -> None:
        player = self._player()
        player.graph = [{"id": 1, "location": [0.0, 0.0, 0.0], "paths": []}]
        player.mates[0]["location"] = (900.0, 0.0, 0.0)
        route = route_over(player)
        sleep = player.sleep

        def downed_on_the_way(seconds: float) -> None:
            sleep(seconds)
            if player.position[0] > 200.0 and "health" not in player.mates[0]:
                player.mates[0]["health"] = 0

        route._sleep = downed_on_the_way
        route_module.join_squad(route, "join")
        self.assertEqual(player.mates[0]["health"], 301)
        walk = [step for step in route.steps if step["step"] == "join"][0]
        self.assertTrue(walk["stopped"])
        self.assertEqual(route.steps[-1]["step"], "join: revive Dom")

    def test_a_mate_downed_while_joining_him_is_revived(self) -> None:
        player = self._player()
        player.mates[0]["health"] = 0
        route = route_over(player)
        route_module.join_squad(route, "join")
        self.assertEqual(player.mates[0]["health"], 301)
        names = [step["step"] for step in route.steps]
        self.assertIn("join: revive Dom", names)
        self.assertEqual(names[-1], "join")

    def test_a_death_reloads_the_checkpoint_and_play_goes_on(self) -> None:
        player = self._player()
        player.alive = False
        player.checkpoint = (0.0, 0.0)

        def saved() -> route_module.Checkpoint:
            return self.NEXT if player.alive and player.now > 30.0 else self.CHECKPOINT

        route = route_over(player)
        route_module.advance_to_next_checkpoint(route, saved)
        self.assertEqual(route.steps[-1]["deaths"], 1)

    def test_a_first_save_counts_as_new(self) -> None:
        player = self._player()
        saves = iter([None, None, self.NEXT])
        self.assertEqual(route_module.advance_to_next_checkpoint(
            route_over(player), lambda: next(saves)), self.NEXT)


class UnblockTest(unittest.TestCase):
    def test_a_stall_runs_the_unblock_action_and_the_walk_resumes(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0, wall_x=100.0)
        route = route_over(player)

        def lift_wall() -> None:
            player.wall_x = None

        route.walk("reach", (500.0, 0.0), radius=40.0, timeout=30.0, unblock=lift_wall)
        self.assertEqual(route.steps[-1]["unblocks"], 1)
        self.assertGreater(player.position[0], 400.0)

    def test_an_unblock_that_never_frees_the_player_fails(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0, wall_x=100.0)
        calls = []
        with self.assertRaisesRegex(RouteFailure, "reach: blocked"):
            route_over(player).walk("reach", (500.0, 0.0), radius=40.0, timeout=60.0,
                                    unblock=lambda: calls.append(1))
        self.assertEqual(len(calls), route_module.MAX_UNBLOCKS)

    def test_a_trigger_unblock_frees_a_player_held_by_a_choice(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0, wall_x=100.0)
        set_pad = player.set_pad

        def choose(fields: dict[str, str]) -> None:
            if fields.get("lt") == "255":
                player.wall_x = None
            set_pad(fields)

        player.set_pad = choose
        route = route_over(player)
        route.walk("reach", (500.0, 0.0), radius=40.0, timeout=30.0,
                   unblock=route.pressing({"lt": "255"}, 0.5))
        self.assertEqual(route.steps[-1]["unblocks"], 1)

    def test_holding_presses_then_releases(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        pressed = []
        set_pad = player.set_pad
        player.set_pad = lambda fields: (pressed.append(fields), set_pad(fields))
        route_over(player).holding("Y", 2.5)()
        self.assertEqual(pressed, [{"buttons": "Y"}])
        self.assertEqual(player.now, 2.5)


class GameTimeTest(unittest.TestCase):
    def test_real_time_passes(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        route = route_over(player)
        route.start_timing()
        player.sleep(100.0)
        self.assertAlmostEqual(route.require_real_time(), 1.0)

    def test_a_fast_simulation_fails(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.time_scale = 2.0
        route = route_over(player)
        route.start_timing()
        player.sleep(100.0)
        with self.assertRaisesRegex(RouteFailure, "game time ran at 2.000"):
            route.require_real_time()

    def test_untimed_routes_refuse(self) -> None:
        with self.assertRaisesRegex(RouteFailure, "never timed"):
            route_over(SimulatedPlayer((0.0, 0.0), 0)).require_real_time()

    def test_a_checkpoint_load_restarts_the_world_clock_without_failing(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.checkpoint = (0.0, 0.0)
        route = route_over(player)
        route.start_timing()
        player.sleep(100.0)
        player.alive = False
        route.respawn("reload", 30.0)
        player.sleep(50.0)
        self.assertAlmostEqual(route.require_real_time(), 1.0)
        self.assertEqual(route.steps[-1]["wall_seconds"], 150.0)

    def test_a_checkpoint_load_does_not_hide_a_fast_simulation(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.checkpoint = (0.0, 0.0)
        player.time_scale = 2.0
        route = route_over(player)
        route.start_timing()
        player.sleep(100.0)
        player.alive = False
        route.respawn("reload", 30.0)
        player.sleep(100.0)
        with self.assertRaisesRegex(RouteFailure, "game time ran at 2.000"):
            route.require_real_time()


class FireTest(unittest.TestCase):
    def test_counts_rounds(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        self.assertGreater(route_over(player).fire("fire", seconds=2.0, attempts=3), 0)

    def test_retries_until_the_trigger_is_released(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        presses = []
        set_pad = player.set_pad

        def held_until_second_press(fields: dict[str, str]) -> None:
            if fields.get("buttons") == "A":
                presses.append(1)
            if len(presses) < 2 and "rt" in fields:
                return
            set_pad(fields)

        player.set_pad = held_until_second_press
        route = route_over(player)
        fired = route.fire("fire", seconds=2.0, attempts=3,
                           before_each=route.holding("A", 0.3))
        self.assertGreater(fired, 0)
        self.assertEqual(route.steps[-1]["attempts"], 2)

    def test_a_weapon_that_counts_nothing_fails(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.set_pad = lambda fields: None  # the trigger never reaches the game
        with self.assertRaisesRegex(RouteFailure, "fire: the weapon counted no rounds in 3 attempts"):
            route_over(player).fire("fire", seconds=2.0, attempts=3)


class AimTest(unittest.TestCase):
    def test_small_errors_still_turn_past_the_dead_zone(self) -> None:
        self.assertGreaterEqual(abs(route_module.aim_stick(300)), route_module.AIM_STICK_FLOOR)
        self.assertLess(route_module.aim_stick(-300), 0)
        self.assertEqual(route_module.aim_stick(10), 0)

    def test_the_error_wraps_across_a_turn(self) -> None:
        player = route_module.Player((0.0, 0.0), 65000, 0, control_pitch=65500)
        yaw_error, pitch_error = route_module.aim_error(player, (100.0, 0.0, -30.0))
        self.assertEqual((yaw_error, pitch_error), (536, 36))

    def test_aim_is_taken_from_the_camera(self) -> None:
        player = route_module.Player((0.0, 0.0), 0, 0, camera=(0.0, 40.0, 0.0))
        yaw_error, _ = route_module.aim_error(player, (400.0, 0.0, -30.0))
        self.assertLess(yaw_error, -500)


class FirefightTest(unittest.TestCase):
    def test_kills_every_hostile(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.add_hostile(7, (1000.0, 300.0, 0.0))
        player.add_hostile(8, (900.0, -600.0, 40.0))
        route = route_over(player)
        self.assertEqual(route.clear_firefight("fight", timeout=120.0, arrival=5.0), 2)
        self.assertTrue(all(hostile["health"] <= 0 for hostile in player.hostiles))
        self.assertEqual(player.stick, (0, 0))
        self.assertFalse(player.firing or player.aiming)

    def test_waits_for_hostiles_to_arrive(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        route = route_over(player)
        sleep = player.sleep

        def arrive(seconds: float) -> None:
            sleep(seconds)
            if player.now > 3.0 and not player.hostiles:
                player.add_hostile(9, (800.0, 0.0, 0.0))

        route._sleep = arrive
        self.assertEqual(route.clear_firefight("fight", timeout=60.0, arrival=10.0), 1)

    def test_an_empty_magazine_is_reloaded_not_swapped(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.weapons["RIGHT"]["fired"] = 60
        player.add_hostile(7, (1000.0, 300.0, 0.0))
        route = route_over(player)
        route.clear_firefight("fight", timeout=120.0, arrival=5.0)
        self.assertEqual(player.held, "RIGHT")
        self.assertLess(player.weapons["RIGHT"]["spare"], 1000)
        fired = [burst["fired"] for burst in route.steps[-1]["bursts"]]
        self.assertEqual(fired[:3], [False, False, True])
        names = [step["step"] for step in route.steps]
        self.assertIn("fight: weapon 2 fired nothing; press A", names)
        self.assertIn("fight: weapon 2 fired nothing; press RB", names)

    def test_an_empty_weapon_is_swapped_for_one_with_ammunition(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.weapons["RIGHT"].update(fired=60, spare=0)
        player.add_hostile(7, (1000.0, 300.0, 0.0))
        route = route_over(player)
        route.clear_firefight("fight", timeout=120.0, arrival=5.0)
        self.assertEqual(player.held, "DOWN")
        names = [step["step"] for step in route.steps]
        self.assertIn("fight: weapon 2 is dry", names)
        self.assertIn("fight: switch to the weapon on DOWN", names)

    def test_a_prompt_under_the_trigger_is_not_a_dry_weapon(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.add_hostile(7, (1000.0, 300.0, 0.0))
        player.prompt = True
        route = route_over(player)
        route.clear_firefight("fight", timeout=120.0, arrival=5.0)
        names = [step["step"] for step in route.steps]
        self.assertIn("fight: weapon 2 fired nothing; press A", names)
        self.assertNotIn("fight: weapon 2 fired nothing; press RB", names)
        self.assertNotIn("fight: weapon 2 is dry", names)
        self.assertEqual(player.weapons["RIGHT"]["spare"], 1000)

    def test_no_ammunition_anywhere_fails(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        for weapon in player.weapons.values():
            weapon.update(fired=weapon["size"], spare=0)
        player.add_hostile(7, (1000.0, 300.0, 0.0))
        with self.assertRaisesRegex(RouteFailure, "fight: every weapon is out of ammunition"):
            route_over(player).clear_firefight("fight", timeout=120.0, arrival=5.0)

    def test_no_hostile_at_all_fails(self) -> None:
        with self.assertRaisesRegex(RouteFailure, "fight: no hostile appeared within 5 s"):
            route_over(SimulatedPlayer((0.0, 0.0), 0)).clear_firefight(
                "fight", timeout=60.0, arrival=5.0)

    def test_an_unhittable_target_yields_to_the_others(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.add_hostile(7, (500.0, 0.0, 0.0), armoured=True)
        player.add_hostile(8, (1500.0, 900.0, 0.0))
        with self.assertRaisesRegex(RouteFailure, r"fight: 1 hostile\(s\) still standing"):
            route_over(player).clear_firefight("fight", timeout=60.0, arrival=5.0)
        self.assertLessEqual(player.hostiles[1]["health"], 0)
        self.assertIn(8, player.shots_on)

    def test_a_hurt_player_recovers_in_cover(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.incoming = 80.0
        player.add_hostile(7, (1000.0, 0.0, 0.0))
        player.add_hostile(8, (1000.0, 700.0, 0.0))
        player.add_hostile(9, (700.0, -900.0, 0.0))
        route = route_over(player)
        route.clear_firefight("fight", timeout=300.0, arrival=5.0)
        self.assertTrue(player.alive)
        bursts = route.steps[-1]["bursts"]
        self.assertTrue(bursts)
        self.assertTrue(all(burst["own_health"] >= route_module.RECOVER_HEALTH for burst in bursts))

    def test_a_death_reloads_the_checkpoint_and_the_fight_resumes(self) -> None:
        player = SimulatedPlayer(route_module.YARD_COVER, 16384)
        player.incoming = 2000.0
        player.checkpoint = (-1606.0, 2573.0)
        player.add_hostile(7, (-1400.0, 4800.0, 0.0))
        route = route_over(player)
        self.assertEqual(route_module.clear_first_firefight(route), 1)
        self.assertLessEqual(player.hostiles[0]["health"], 0)
        names = [step["step"] for step in route.steps]
        self.assertIn("reload the yard checkpoint after death 1", names)
        self.assertEqual(names[-1], "clear the yard's first firefight, attempt 2")

    def test_deaths_past_the_bound_fail(self) -> None:
        player = SimulatedPlayer(route_module.YARD_COVER, 16384)
        player.incoming = 2000.0
        player.incoming_after_respawn = 2000.0
        player.checkpoint = (-1606.0, 2573.0)
        player.add_hostile(7, (-1400.0, 4800.0, 0.0))
        with self.assertRaisesRegex(RouteFailure, "attempt 4: the player died"):
            route_module.clear_first_firefight(route_over(player))

    def test_a_death_screen_that_never_respawns_fails(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.alive = False
        with self.assertRaisesRegex(RouteFailure, "reload: no pawn within 30 s"):
            route_over(player).respawn("reload", 30.0)

    def test_death_fails_the_step(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.incoming = 2000.0
        player.add_hostile(7, (1000.0, 0.0, 0.0))
        route = route_over(player)
        with self.assertRaisesRegex(RouteFailure, "fight: the player died"):
            route.clear_firefight("fight", timeout=60.0, arrival=5.0)
        self.assertEqual(route.steps[-1]["failure"], "fight: the player died")
        self.assertEqual(route.steps[-1]["hostiles"], 1)


if __name__ == "__main__":
    unittest.main()
