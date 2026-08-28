#!/usr/bin/env python3
"""Resolve this project's entry point to the canonical shared info tool.

The information system is maintained once in the sibling ``shared/re-harness``
checkout. This file exists so project work can consistently start with
``tools/info.py brief ...`` without copying that implementation into every
project.

Discovery order:

1. ``RE_HARNESS_REPO`` names the shared/re-harness checkout explicitly.
2. ``SHARED_DIR`` names its parent shared checkout.
3. The conventional ``../../shared/re-harness`` sibling of this repository.

The delegated tool always runs with the project root as its working directory;
the canonical tool resolves this project's ``docs/info`` relative to cwd.
"""

import os
from pathlib import Path
import runpy
import sys


def _candidate_paths(project_root: Path, environ: dict[str, str]) -> list[Path]:
    candidates: list[Path] = []
    seen: set[Path] = set()

    def add(path: Path) -> None:
        resolved = path.expanduser().resolve()
        if resolved not in seen:
            seen.add(resolved)
            candidates.append(resolved)

    harness_repo = environ.get("RE_HARNESS_REPO")
    if harness_repo:
        add(Path(harness_repo) / "tools" / "info.py")

    shared_dir = environ.get("SHARED_DIR")
    if shared_dir:
        add(Path(shared_dir) / "re-harness" / "tools" / "info.py")

    add(project_root.parent.parent / "shared" / "re-harness" / "tools" / "info.py")
    return candidates


def find_canonical_info(project_root: Path,
                        environ: dict[str, str] | None = None) -> tuple[Path | None, list[Path]]:
    """Return the first existing canonical tool and all paths that were checked."""

    checked = _candidate_paths(project_root, os.environ if environ is None else environ)
    return next((path for path in checked if path.is_file()), None), checked


def main() -> int:
    project_root = Path(__file__).resolve().parents[1]
    canonical, checked = find_canonical_info(project_root)
    if canonical is None:
        print(
            "info: canonical shared/re-harness tools/info.py was not found. "
            "Set RE_HARNESS_REPO to that checkout or place it at "
            "../../shared/re-harness relative to this repository. Checked:",
            file=sys.stderr,
        )
        for path in checked:
            print(f"  {path}", file=sys.stderr)
        return 2

    original_cwd = Path.cwd()
    try:
        os.chdir(project_root)
        runpy.run_path(str(canonical), run_name="__main__")
    finally:
        os.chdir(original_cwd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
