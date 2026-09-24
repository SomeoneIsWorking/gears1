"""Shortest walks over a navigation graph shaped like /api/navigation's."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tools.navigation import NavigationError, NavigationGraph  # noqa: E402


WALK = ("walk",)


def graph(points):
    """Points whose paths are (end, distance) walks or (end, distance, kind)."""
    return NavigationGraph.from_json(
        {"points": [{"id": i, "kind": "path", "yaw": 0, "location": loc,
                     "paths": [path if len(path) == 3 else [*path, "walk"] for path in paths]}
                    for i, loc, paths in points]}
    )


def ids(hops):
    return [hop.point.id for hop in hops]


# A square: 1 -> 2 -> 4 is short; 1 -> 3 -> 4 is long; 5 is on another floor.
SQUARE = graph(
    [
        (1, [0, 0, 0], [[2, 100], [3, 100]]),
        (2, [100, 0, 0], [[4, 100], [1, 100]]),
        (3, [0, 100, 0], [[4, 500], [1, 50, "mantle"]]),
        (4, [100, 100, 0], [[2, 100]]),
        (5, [10, 10, 400], []),
    ]
)


class ShortestPathTest(unittest.TestCase):
    def test_takes_the_shorter_way(self):
        self.assertEqual(ids(SQUARE.shortest_path(1, 4, WALK)), [1, 2, 4])

    def test_follows_one_way_paths(self):
        # 3 -> 4 is one way: the way back goes around through 2 and 1.
        self.assertEqual(ids(SQUARE.shortest_path(4, 3, WALK)), [4, 2, 1, 3])
        with self.assertRaisesRegex(NavigationError,
                                    "unreachable from 0x1 over 4 points reached by walk paths"):
            SQUARE.shortest_path(1, 5, WALK)

    def test_follows_only_the_kinds_given(self):
        # 3 -> 1 is only a mantle: a walk goes round through 4 and 2.
        self.assertEqual(ids(SQUARE.shortest_path(3, 1, WALK)), [3, 4, 2, 1])
        hops = SQUARE.shortest_path(3, 1, ("walk", "mantle"))
        self.assertEqual([(hop.point.id, hop.kind) for hop in hops], [(3, None), (1, "mantle")])

    def test_start_is_goal(self):
        self.assertEqual(ids(SQUARE.shortest_path(2, 2, WALK)), [2])

    def test_ignores_paths_to_unlisted_points(self):
        g = graph([(1, [0, 0, 0], [[9, 10], [2, 50]]), (2, [50, 0, 0], [])])
        self.assertEqual(ids(g.shortest_path(1, 2, WALK)), [1, 2])

    def test_refuses_unknown_points(self):
        with self.assertRaisesRegex(NavigationError, "not in the level"):
            SQUARE.shortest_path(1, 42, WALK)


class NearestTest(unittest.TestCase):
    def test_nearest_on_the_same_floor(self):
        self.assertEqual(SQUARE.nearest((12.0, 9.0, 0.0), max_height=100.0).id, 1)

    def test_other_floor_when_within_height(self):
        self.assertEqual(SQUARE.nearest((12.0, 9.0, 380.0), max_height=100.0).id, 5)

    def test_refuses_when_no_point_is_near_in_height(self):
        with self.assertRaisesRegex(NavigationError, "no navigation point"):
            SQUARE.nearest((0.0, 0.0, 2000.0), max_height=100.0)

    def test_empty_level_refuses(self):
        with self.assertRaisesRegex(NavigationError, "no navigation points"):
            graph([])


class ReachableTest(unittest.TestCase):
    def test_follows_only_the_given_kinds(self):
        self.assertEqual(SQUARE.reachable(1, {"walk"}), {1, 2, 3, 4})
        self.assertEqual(SQUARE.reachable(3, {"mantle"}), {3, 1})
        self.assertEqual(SQUARE.reachable(5, {"walk", "mantle"}), {5})

    def test_an_unknown_start_refuses(self):
        with self.assertRaisesRegex(NavigationError, "0x9 is not in the level"):
            SQUARE.reachable(9, {"walk"})


class KindTest(unittest.TestCase):
    def test_lists_the_points_of_a_kind(self):
        level = NavigationGraph.from_json({"points": [
            {"id": 1, "kind": "path", "yaw": 0, "location": [0, 0, 0], "paths": []},
            {"id": 2, "kind": "cover", "yaw": 16384, "location": [50, 0, 0], "paths": []},
            {"id": 3, "kind": "0x82095000", "yaw": 0, "location": [90, 0, 0], "paths": []},
        ]})
        self.assertEqual([point.id for point in level.of_kind("cover")], [2])
        self.assertEqual(level.point(3).kind, "0x82095000")
        self.assertEqual(level.point(2).yaw, 16384)

    def test_a_point_without_a_kind_refuses(self):
        with self.assertRaises(KeyError):
            NavigationGraph.from_json({"points": [{"id": 1, "yaw": 0, "location": [0, 0, 0],
                                                   "paths": []}]})


if __name__ == "__main__":
    unittest.main()
