"""The combat route's world model and steering geometry, without a product."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tools import combat_world as world
from tools.combat_world import stick_toward
from tools.navigation import NavigationGraph

# A corridor of path nodes with cover slots at x 300, facing +x, and at x -200,
# facing -x.
COVER_GRAPH = [
    {"id": 1, "location": [0.0, 0.0, 0.0], "paths": [[2, 300, "walk"], [3, 200, "walk"]]},
    {"id": 2, "kind": "cover", "yaw": 0, "location": [300.0, 0.0, 0.0],
     "paths": [[1, 300, "walk"]]},
    {"id": 3, "kind": "cover", "yaw": 32768, "location": [-200.0, 0.0, 0.0],
     "paths": [[1, 200, "walk"]]},
]


class StickTest(unittest.TestCase):
    def test_goal_ahead_is_full_forward(self) -> None:
        self.assertEqual(stick_toward((0.0, 0.0), 0, (100.0, 0.0)), (0, 32767))

    def test_goal_a_quarter_turn_clockwise_is_full_right(self) -> None:
        # Facing +x, +y is to the right in the left-handed world.
        self.assertEqual(stick_toward((0.0, 0.0), 0, (0.0, 100.0)), (32767, 0))

    def test_yaw_is_subtracted(self) -> None:
        # Facing +y (yaw a quarter turn), a goal at +y is straight ahead.
        self.assertEqual(stick_toward((0.0, 0.0), 16384, (0.0, 100.0)), (0, 32767))


class AimTest(unittest.TestCase):
    def test_small_errors_still_turn_past_the_dead_zone(self) -> None:
        self.assertGreaterEqual(abs(world.aim_stick(300)), world.AIM_STICK_FLOOR)
        self.assertLess(world.aim_stick(-300), 0)
        self.assertEqual(world.aim_stick(10), 0)

    def test_the_error_wraps_across_a_turn(self) -> None:
        player = world.Player((0.0, 0.0), 65000, 0, control_pitch=65500)
        yaw_error, pitch_error = world.aim_error(player, (100.0, 0.0, -30.0))
        self.assertEqual((yaw_error, pitch_error), (536, 36))

    def test_aim_is_taken_from_the_camera(self) -> None:
        player = world.Player((0.0, 0.0), 0, 0, camera=(0.0, 40.0, 0.0))
        yaw_error, _ = world.aim_error(player, (400.0, 0.0, -30.0))
        self.assertLess(yaw_error, -500)


class CoverTest(unittest.TestCase):
    def _graph(self, points: list[dict[str, object]]) -> NavigationGraph:
        return NavigationGraph.from_json(
            {"points": [{"kind": "path", "yaw": 0, **point} for point in points]})

    def _player(self, position: tuple[float, float], height: float = 0.0) -> world.Player:
        return world.Player(position, 0, None, height=height)

    def _hostile(self, x: float, y: float) -> world.Pawn:
        return world.Pawn(7, (x, y, 0.0), 250, 1, False)

    def test_a_slot_facing_the_hostiles_beats_a_nearer_one(self) -> None:
        slot = world.choose_cover(self._graph(COVER_GRAPH), self._player((-100.0, 0.0)),
                                         [self._hostile(1500.0, 0.0)])
        self.assertEqual(slot.id, 2)

    def test_skips_a_slot_no_walk_reaches(self) -> None:
        unreachable = {"id": 5, "kind": "cover", "yaw": 0, "location": [150.0, 0.0, 0.0],
                       "paths": []}
        slot = world.choose_cover(self._graph([*COVER_GRAPH, unreachable]),
                                         self._player((-50.0, 0.0)),
                                         [self._hostile(1500.0, 0.0)])
        self.assertEqual(slot.id, 2)

    def test_takes_the_nearest_of_the_slots_facing_them(self) -> None:
        graph = self._graph([{**point, "yaw": 0} for point in COVER_GRAPH])
        slot = world.choose_cover(graph, self._player((-100.0, 0.0)),
                                         [self._hostile(1500.0, 0.0)])
        self.assertEqual(slot.id, 3)

    def test_the_slot_facing_most_hostiles_wins(self) -> None:
        slot = world.choose_cover(self._graph(COVER_GRAPH), self._player((200.0, 0.0)),
                                         [self._hostile(-1500.0, 0.0), self._hostile(-1400.0, 300.0),
                                          self._hostile(1500.0, 0.0)])
        self.assertEqual(slot.id, 3)

    def test_skips_a_slot_a_hostile_stands_close_to(self) -> None:
        graph = self._graph([{**point, "yaw": 0} for point in COVER_GRAPH])
        slot = world.choose_cover(graph, self._player((100.0, 0.0)),
                                         [self._hostile(650.0, 0.0)])
        self.assertEqual(slot.id, 3)

    def test_skips_slots_facing_away_on_another_floor_and_out_of_reach(self) -> None:
        graph = self._graph([
            {"id": 1, "kind": "cover", "yaw": 0, "location": [100.0, 0.0, 400.0], "paths": []},
            {"id": 2, "kind": "cover", "yaw": 0, "location": [900.0, 0.0, 0.0], "paths": []},
            {"id": 4, "kind": "cover", "yaw": 32768, "location": [0.0, 100.0, 0.0], "paths": []},
            {"id": 3, "location": [50.0, 0.0, 0.0], "paths": []},
        ])
        self.assertIsNone(world.choose_cover(graph, self._player((0.0, 0.0)),
                                                    [self._hostile(3000.0, 0.0)]))


if __name__ == "__main__":
    unittest.main()
