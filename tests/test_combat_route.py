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
    """Moves along the stick direction relative to its yaw, like the engine's walk input."""

    def __init__(self, position: tuple[float, float], yaw: int, speed: float = 60.0,
                 wall_x: float | None = None) -> None:
        self.position = position
        self.yaw = yaw
        self.speed = speed
        self.wall_x = wall_x
        self.stick = (0, 0)
        self.rounds = 0
        self.firing = False
        self.alive = True
        self.now = 0.0
        self.time_scale = 1.0

    # Pad protocol
    def player(self) -> dict[str, object]:
        if not self.alive:
            return {"control_yaw": self.yaw, "camera_yaw": self.yaw,
                    "world_seconds": self.now * self.time_scale, "pawn": None}
        return {
            "control_yaw": self.yaw,
            "camera_yaw": self.yaw,
            "world_seconds": self.now * self.time_scale,
            "pawn": {"location": [*self.position, 0.0], "magazine_rounds_fired": self.rounds},
        }

    def set_pad(self, fields: dict[str, str]) -> None:
        self.stick = (int(fields.get("lx", "0")), int(fields.get("ly", "0")))
        self.firing = fields.get("rt") == "255"

    def release(self) -> None:
        self.stick = (0, 0)
        self.firing = False

    # Clock and sleep advance the simulation.
    def clock(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds
        if self.firing:
            self.rounds += round(20 * seconds)
        lx, ly = (value / route_module.STICK_LIMIT for value in self.stick)
        yaw = self.yaw * 2.0 * math.pi / route_module.YAW_UNITS_PER_TURN
        # Forward is along yaw; right is a quarter turn toward +y in the left-handed world.
        dx = ly * math.cos(yaw) - lx * math.sin(yaw)
        dy = ly * math.sin(yaw) + lx * math.cos(yaw)
        x = self.position[0] + dx * self.speed * seconds * 10
        y = self.position[1] + dy * self.speed * seconds * 10
        if self.wall_x is not None and x > self.wall_x:
            x = self.wall_x
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
        start = route.clock()
        player.sleep(100.0)
        self.assertAlmostEqual(route.require_real_time(start), 1.0)

    def test_a_fast_simulation_fails(self) -> None:
        player = SimulatedPlayer((0.0, 0.0), 0)
        player.time_scale = 2.0
        route = route_over(player)
        start = route.clock()
        player.sleep(100.0)
        with self.assertRaisesRegex(RouteFailure, "game time ran at 2.000"):
            route.require_real_time(start)


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


if __name__ == "__main__":
    unittest.main()
