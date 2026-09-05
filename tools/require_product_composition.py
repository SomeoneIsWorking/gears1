#!/usr/bin/env python3
"""Fail at the one deliberately missing GearsUE3 product-composition boundary."""

from __future__ import annotations

import sys


def main() -> int:
    print(
        "GearsUE3 product unavailable: the authenticated full-image adapter and "
        "runtime-service composition are not yet wired over shared/x360port. "
        "The retired generated-code product cannot be generated, built, or selected.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
