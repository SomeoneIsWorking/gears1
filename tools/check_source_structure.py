#!/usr/bin/env python3
"""Enforce source-file size boundaries and ratchet legacy monoliths down."""

import subprocess
import sys
from pathlib import Path


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".py", ".sh"}
DEFAULT_MAX_LINES = 1000
LEGACY_LIMITS = {
    "runtime/gpu_draw.cpp": 3913,
    "runtime/vd_null_gpu.cpp": 3467,
    "runtime/guest_probes.cpp": 2182,
    "runtime/gpu_draw_probe.cpp": 1513,
    "runtime/gpu_draw_xlate.cpp": 1468,
    "runtime/gpu_present.cpp": 1456,
    "tools/layer_compare.py": 1085,
    "tools/gfr_to_xtr.py": 1002,
}


def violations(counts):
    found = []
    for name, count in sorted(counts.items()):
        limit = LEGACY_LIMITS.get(name, DEFAULT_MAX_LINES)
        if count > limit:
            found.append(f"{name}: {count} lines exceeds its {limit}-line limit")
        elif name in LEGACY_LIMITS and count < limit:
            found.append(
                f"{name}: reduced to {count} lines; ratchet its declared "
                f"{limit}-line limit down to {count}")
    return found


def source_counts(root):
    listed = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=root, check=True, text=True, capture_output=True).stdout.splitlines()
    paths = [name for name in listed if Path(name).suffix in SOURCE_SUFFIXES]
    if not paths:
        raise RuntimeError("git listed zero source files; structure was not checked")
    counts = {}
    for name in paths:
        path = root / name
        if path.is_file():
            with path.open("rb") as source:
                counts[name] = sum(1 for _ in source)
    if not counts:
        raise RuntimeError("zero source files were readable; structure was not checked")
    return counts


def selftest():
    cases = [
        ({"new.cpp": DEFAULT_MAX_LINES}, []),
        ({"new.cpp": DEFAULT_MAX_LINES + 1}, ["exceeds"]),
        ({"runtime/gpu_draw.cpp": LEGACY_LIMITS["runtime/gpu_draw.cpp"] + 1},
         ["exceeds"]),
        ({"runtime/gpu_draw.cpp": LEGACY_LIMITS["runtime/gpu_draw.cpp"] - 1},
         ["ratchet"]),
    ]
    for counts, required in cases:
        text = "\n".join(violations(counts))
        if any(word not in text for word in required) or (not required and text):
            print(f"FAIL: counts {counts} produced {text!r}")
            return 1
    print("source structure selftest passed: accepts the boundary and rejects "
          "new growth, legacy growth, and an unratcheted reduction")
    return 0


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    if len(argv) != 1:
        print(__doc__)
        return 2
    try:
        counts = source_counts(Path(__file__).resolve().parents[1])
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(f"REFUSING: {error}")
        return 2
    found = violations(counts)
    if found:
        print("source structure check failed:")
        for problem in found:
            print(f"  {problem}")
        return 1
    print(f"source structure check passed: {len(counts)} files; new-file cap "
          f"{DEFAULT_MAX_LINES}, {len(LEGACY_LIMITS)} legacy ratchets")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
