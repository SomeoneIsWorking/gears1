#!/usr/bin/env python3
"""Compile a clean tracked GLSL source into a deterministic C++ SPIR-V header."""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


FUNCTION_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


def within(root: Path, path: Path) -> Path:
    resolved = path.resolve()
    try:
        return resolved.relative_to(root)
    except ValueError as error:
        raise ValueError(f"path is outside the repository: {path}") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("function")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    source = (root / args.source).resolve() if not args.source.is_absolute() else args.source
    output = (root / args.output).resolve() if not args.output.is_absolute() else args.output
    source_relative = within(root, source)
    output_relative = within(root, output)
    if not source.is_file():
        parser.error(f"source does not exist: {source_relative}")
    if not FUNCTION_NAME.fullmatch(args.function):
        parser.error(f"invalid C++ function name: {args.function}")

    compiler = shutil.which("glslangValidator")
    if compiler is None:
        parser.error("glslangValidator is not installed")

    stage = {".vert": "vert", ".comp": "comp"}.get(source.suffix, "frag")
    scratch = root / "scratch"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gen_native_spv.", dir=scratch) as temporary:
        module = Path(temporary) / "out.spv"
        subprocess.run(
            [compiler, "-V", "--target-env", "vulkan1.1", "-S", stage,
             str(source), "-o", str(module)],
            check=True,
        )
        data = module.read_bytes()

    if len(data) % 4 != 0:
        raise RuntimeError("SPIR-V output is not a whole number of words")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if not words or words[0] != 0x07230203:
        raise RuntimeError("compiler did not emit little-endian SPIR-V")
    body = ",".join(f"0x{word:08x}u" for word in words)
    output.write_text(
        f"""// GENERATED from {source_relative} by tools/gen_native_spv.py -- do not edit.
// Regenerate: tools/gen_native_spv.py {source_relative} {output_relative} {args.function}
#pragma once
#include <cstdint>
#include <vector>

namespace gears::native {{
inline const std::vector<uint32_t>& {args.function}() {{
    static const std::vector<uint32_t> code = {{{body}}};
    return code;
}}
}} // namespace gears::native
""",
        encoding="utf-8",
    )
    print(f"{output_relative}: {len(words)} SPIR-V words from {source_relative}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
