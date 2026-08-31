#!/usr/bin/env python3
"""Locked-environment entry point for fresh-clone preparation and launch."""

from tools.gearsue3_bootstrap.launcher import entrypoint


if __name__ == "__main__":
    raise SystemExit(entrypoint())
