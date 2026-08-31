#!/usr/bin/env python3
"""Refresh only the public context header in an existing generated PPC tree."""

from __future__ import annotations

import argparse
from pathlib import Path


class RefreshError(RuntimeError):
    """The requested generated-header refresh is unsafe or incomplete."""


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def generated_header(source: Path) -> str:
    return '#pragma once\n#include "ppc_config.h"\n\n' + source.read_text(encoding="utf-8")


def refresh(ppc_directory: Path, root: Path) -> bool:
    scratch = (root / "scratch").resolve()
    directory = ppc_directory.resolve()
    try:
        directory.relative_to(scratch)
    except ValueError as error:
        raise RefreshError(f"generated PPC directory must be under {scratch}: {directory}") from error
    destination = directory / "ppc_context.h"
    if not destination.is_file():
        raise RefreshError(f"generated PPC header is missing: {destination}")
    source = root / "extern/XenonRecomp/XenonUtils/ppc_context.h"
    contents = generated_header(source)
    if destination.read_text(encoding="utf-8") == contents:
        return False
    destination.write_text(contents, encoding="utf-8")
    return True


def main() -> int:
    root = repository_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ppc-dir",
        type=Path,
        required=True,
        help="existing generated PPC directory under scratch/",
    )
    arguments = parser.parse_args()
    directory = arguments.ppc_dir
    if not directory.is_absolute():
        directory = root / directory
    changed = refresh(directory, root)
    print(f"recompiler header {'refreshed' if changed else 'already current'}: {directory}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RefreshError as error:
        raise SystemExit(f"REFUSING: {error}")
