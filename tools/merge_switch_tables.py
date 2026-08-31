#!/usr/bin/env python3
"""Append hand-authored switch tables to the ones XenonAnalyse recovers.

XenonAnalyse regenerates its table file from the image, so anything added to
that file by hand is destroyed the next time it runs. That is not a
hypothetical: the entry in config/gears_switch_tables.extra.toml is load
bearing -- without it the render-command executor's dispatch is recompiled
wrongly and the title crashes -- and it lived only in the generated file, on
one machine, until this script existed.

Generated output stays in scratch/ where it belongs; the authored entries are
tracked in config/ and merged in here, so regenerating can no longer lose them.

    tools/merge_switch_tables.py <generated> <extra> <output>
"""
import os
from pathlib import Path
import sys


def merge_switch_tables(generated: Path, extra: Path, output: Path) -> None:
    """Atomically merge generated discovery with the tracked authored table."""

    body = generated.read_text(encoding="utf-8")
    authored = extra.read_text(encoding="utf-8")
    if not body.endswith("\n"):
        body += "\n"

    output.parent.mkdir(parents=True, exist_ok=True)
    merged = (
        body
        + "\n# ---- merged from "
        + extra.as_posix()
        + " by tools/merge_switch_tables.py ----\n"
        + authored
    )
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    temporary.write_text(merged, encoding="utf-8")
    os.replace(temporary, output)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    generated, extra, output = (Path(value) for value in sys.argv[1:4])
    merge_switch_tables(generated, extra, output)

    print("merged %s + %s -> %s" % (generated, extra, output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
