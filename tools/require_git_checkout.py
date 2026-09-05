#!/usr/bin/env python3
"""Resolve one immutable clean Git dependency for the CMake build graph."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from gearsue3_bootstrap.dependency_checkout import (
    DependencyCheckoutError,
    require_git_checkout,
)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--required-file", type=Path)
    return parser.parse_args()


def main() -> int:
    selected = arguments()
    try:
        checkout = require_git_checkout(
            selected.root,
            selected.revision,
            selected.label,
            required_file=selected.required_file,
        )
    except DependencyCheckoutError as error:
        print(error, file=sys.stderr)
        return 1
    print(checkout.revision)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
