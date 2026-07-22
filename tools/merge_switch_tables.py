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
import sys


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    generated, extra, output = sys.argv[1:4]

    with open(generated) as handle:
        body = handle.read()
    with open(extra) as handle:
        authored = handle.read()

    if not body.endswith("\n"):
        body += "\n"

    with open(output, "w") as handle:
        handle.write(body)
        handle.write("\n# ---- merged from ")
        handle.write(extra)
        handle.write(" by tools/merge_switch_tables.py ----\n")
        handle.write(authored)

    print("merged %s + %s -> %s" % (generated, extra, output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
