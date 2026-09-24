"""Shortest walks over a level's navigation graph, as /api/navigation reports it.

The engine's own path finder walks these points; a route that follows the same
paths reaches places a straight-line walk runs into walls on the way to. Each
path has a kind: "walk", "mantle" (over low cover), or an unknown ReachSpec
class named by its vtable; a search follows only the kinds it is given.
"""

from __future__ import annotations

import heapq
import math
from collections.abc import Collection
from dataclasses import dataclass


class NavigationError(RuntimeError):
    """The graph cannot answer the question asked of it."""


@dataclass(frozen=True)
class Path:
    end: int
    distance: int
    kind: str


@dataclass(frozen=True)
class Point:
    id: int
    location: tuple[float, float, float]
    paths: tuple[Path, ...]


@dataclass(frozen=True)
class Hop:
    """One point of a walk and the kind of path that arrives there (None at the start)."""

    point: Point
    kind: str | None


class NavigationGraph:
    def __init__(self, points: list[Point]) -> None:
        if not points:
            raise NavigationError("the level lists no navigation points")
        self._points = {point.id: point for point in points}

    @classmethod
    def from_json(cls, data: dict[str, object]) -> NavigationGraph:
        points = [
            Point(
                id=int(entry["id"]),
                location=tuple(float(v) for v in entry["location"]),
                paths=tuple(Path(int(end), int(length), str(kind))
                            for end, length, kind in entry["paths"]),
            )
            for entry in data["points"]
        ]
        return cls(points)

    def __len__(self) -> int:
        return len(self._points)

    def point(self, point_id: int) -> Point:
        return self._points[point_id]

    def nearest(self, location: tuple[float, float, float], max_height: float) -> Point:
        """The point closest across the ground whose height is within max_height."""
        candidates = [
            point
            for point in self._points.values()
            if abs(point.location[2] - location[2]) <= max_height
        ]
        if not candidates:
            raise NavigationError(
                f"no navigation point within {max_height} units of height {location[2]:.0f}"
            )
        return min(candidates, key=lambda point: _ground(point.location, location))

    def shortest_path(self, start: int, goal: int, kinds: Collection[str]) -> list[Hop]:
        """Dijkstra over the lengths of paths of the given kinds; refuses an unreachable goal."""
        if start not in self._points or goal not in self._points:
            raise NavigationError(f"point {start:#x} or {goal:#x} is not in the level")
        frontier = [(0.0, start)]
        cost = {start: 0.0}
        previous: dict[int, tuple[int, str]] = {}
        while frontier:
            spent, current = heapq.heappop(frontier)
            if current == goal:
                return self._unwind(previous, start, goal)
            if spent > cost[current]:
                continue
            for path in self._points[current].paths:
                if path.kind not in kinds or path.end not in self._points:
                    continue
                through = spent + max(path.distance, 1)
                if through < cost.get(path.end, math.inf):
                    cost[path.end] = through
                    previous[path.end] = (current, path.kind)
                    heapq.heappush(frontier, (through, path.end))
        raise NavigationError(
            f"point {goal:#x} is unreachable from {start:#x} over {len(cost)} points "
            f"reached by {', '.join(sorted(kinds))} paths"
        )

    def _unwind(self, previous: dict[int, tuple[int, str]], start: int, goal: int) -> list[Hop]:
        hops = []
        current = goal
        while current != start:
            before, kind = previous[current]
            hops.append(Hop(self._points[current], kind))
            current = before
        hops.append(Hop(self._points[start], None))
        return list(reversed(hops))


def _ground(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return math.hypot(a[0] - b[0], a[1] - b[1])
