#!/usr/bin/env python3
"""Remove explicitly selected build trees and retired scratch build roots."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_ROOT = REPO_ROOT / "build"
LEGACY_BUILD_ROOTS = (
    REPO_ROOT / "scratch/build",
    REPO_ROOT / "scratch/oracle/oracle-build",
)


class CleanBuildError(RuntimeError):
    """A requested cleanup target is not an exact owned build tree."""


def build_target(name: str) -> Path:
    if not name or Path(name).name != name or name in {".", ".."}:
        raise CleanBuildError(
            f"build target must be one direct child name below {BUILD_ROOT}: {name!r}"
        )
    return BUILD_ROOT / name


def remove_tree(path: Path, allowed: tuple[Path, ...], *, dry_run: bool) -> bool:
    target = path.resolve()
    permitted = tuple(candidate.resolve() for candidate in allowed)
    if target not in permitted:
        raise CleanBuildError(f"cleanup target is not an exact owned build tree: {path}")
    if path.is_symlink():
        raise CleanBuildError(f"cleanup target is a symlink: {path}")
    if not path.exists():
        print(f"clean-build: absent {path}")
        return False
    if not path.is_dir():
        raise CleanBuildError(f"cleanup target is not a directory: {path}")
    print(f"clean-build: {'would remove' if dry_run else 'removing'} {path}")
    if not dry_run:
        shutil.rmtree(path)
    return True


def parse_arguments(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build",
        action="append",
        default=[],
        metavar="NAME",
        help="remove one exact direct child of the top-level build directory",
    )
    parser.add_argument(
        "--legacy",
        action="store_true",
        help="remove only the three retired build roots under scratch",
    )
    parser.add_argument("--dry-run", action="store_true")
    selected = parser.parse_args(arguments)
    if not selected.build and not selected.legacy:
        parser.error("select at least one cleanup scope")
    return selected


def main(arguments: list[str] | None = None) -> int:
    selected = parse_arguments(sys.argv[1:] if arguments is None else arguments)
    try:
        requested = tuple(build_target(name) for name in selected.build)
        allowed = requested + (LEGACY_BUILD_ROOTS if selected.legacy else ())
        for path in allowed:
            remove_tree(path, allowed, dry_run=selected.dry_run)
    except CleanBuildError as error:
        print(f"clean-build: REFUSING: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
