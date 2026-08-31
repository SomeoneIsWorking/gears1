"""Strict machine-local environment loading without shell evaluation."""

from __future__ import annotations

import os
from collections.abc import Mapping
from pathlib import Path

ALLOWED_NAMES = frozenset({"GEARS_ISO", "GEARS_GAME_DIR", "GEARS_BUILD_DIR"})


class EnvironmentError(RuntimeError):
    """The machine-local environment file is malformed or ambiguous."""


def environment_file(
    repo_root: Path,
    environ: Mapping[str, str] | None = None,
    env_file: Path | None = None,
) -> Path:
    """Resolve the one environment file consumed by the shipping launcher."""

    selected = env_file or Path(
        (os.environ if environ is None else environ).get(
            "GEARS_ENV_FILE", repo_root / ".env"
        )
    )
    return selected if selected.is_absolute() else repo_root / selected


def load_environment(
    repo_root: Path,
    environ: Mapping[str, str] | None = None,
    env_file: Path | None = None,
) -> dict[str, str]:
    result = dict(os.environ if environ is None else environ)
    selected_file = environment_file(repo_root, result, env_file)
    if not selected_file.exists():
        return result
    if not selected_file.is_file():
        raise EnvironmentError(f"environment path is not a file: {selected_file}")
    seen: set[str] = set()
    for line_number, raw_line in enumerate(
        selected_file.read_text(encoding="utf-8").splitlines(), 1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        name, separator, value = line.partition("=")
        name = name.strip()
        if separator == "" or name not in ALLOWED_NAMES:
            continue
        if name in seen:
            raise EnvironmentError(
                f"{selected_file}:{line_number}: {name} is defined more than once"
            )
        seen.add(name)
        value = value.strip()
        if value[:1] in {"'", '"'}:
            quote = value[0]
            if len(value) < 2 or value[-1] != quote:
                raise EnvironmentError(
                    f"{selected_file}:{line_number}: unterminated quoted {name}"
                )
            value = value[1:-1]
        if value and name not in result:
            result[name] = value
    return result
