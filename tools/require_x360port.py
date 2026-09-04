#!/usr/bin/env python3
"""Fail at the one deliberately missing GearsUE3 product boundary."""

from __future__ import annotations

import sys


def main() -> int:
    print(
        "GearsUE3 product unavailable: shared/x360port has not yet embedded "
        "Xenia's Xenon dynarec and exposed the required executor boundary. "
        "The retired generated-code product cannot be generated, built, or selected.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
