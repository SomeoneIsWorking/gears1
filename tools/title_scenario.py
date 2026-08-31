"""Read title-owned startup state for paired diagnostic scenarios."""

from __future__ import annotations

from pathlib import Path

import startup_map


class ScenarioError(RuntimeError):
    """The title configuration cannot identify one unambiguous startup path."""


def startup_map_name(game_dir: Path) -> str:
    path = game_dir / startup_map.DEFAULT_CONFIG
    if not path.is_file():
        raise ScenarioError(f"title startup configuration is missing: {path}")
    try:
        files = startup_map.parse(path.read_bytes())
    except (OSError, ValueError) as error:
        raise ScenarioError(f"cannot parse title startup configuration {path}: {error}") from error
    values = {
        url["LocalMap"]
        for _name, body in files
        if (url := startup_map.read_url(body)) and "LocalMap" in url
    }
    if not values:
        raise ScenarioError(f"no [URL] LocalMap found in {path}")
    if len(values) != 1:
        raise ScenarioError(f"conflicting [URL] LocalMap values in {path}: {sorted(values)}")
    return values.pop()


def is_direct_boot(map_name: str) -> bool:
    return not map_name.casefold().startswith("warstart")
