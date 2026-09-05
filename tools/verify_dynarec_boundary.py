#!/usr/bin/env python3
"""Build and execute the asset-free Gears/x360port discriminator."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from gearsue3_bootstrap.paths import build_directory
from gearsue3_bootstrap.process import CommandRunner

ROOT = Path(__file__).resolve().parents[1]


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--x360port-root", required=True, type=Path)
    parser.add_argument(
        "--build-dir", default=Path("build/dynarec-boundary"), type=Path
    )
    parser.add_argument("--expected-machine", choices=("x86_64", "arm64"))
    parser.add_argument("--parallel", type=int, default=2)
    return parser.parse_args()


def main() -> int:
    selected = arguments()
    if selected.parallel < 1:
        raise ValueError("--parallel must be positive")
    x360port_root = selected.x360port_root.resolve()
    if not (x360port_root / "CMakeLists.txt").is_file():
        raise ValueError(f"x360port root is not a source checkout: {x360port_root}")
    output = build_directory(
        ROOT, str(selected.build_dir), ROOT / "build/dynarec-boundary"
    )
    # The framework owns host compilers and Xenia's nested dependency preparation.
    sys.path.insert(0, str(x360port_root / "tools"))
    from build_support import (
        cmake_child_environment,
        compiler_names,
        require_machine,
        require_program,
    )
    from xenia_dependencies import prepare_xenia_dependencies

    require_machine(selected.expected_machine)
    c_name, cxx_name = compiler_names()
    c_compiler, cxx_compiler = require_program(c_name), require_program(cxx_name)
    dependency_options = prepare_xenia_dependencies(ROOT / "extern/xenia", output)
    cmake = require_program("cmake")
    runner = CommandRunner()
    runner.run(
        [
            cmake,
            "-S",
            ROOT,
            "-B",
            output,
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
            f"-DCMAKE_C_COMPILER={c_compiler}",
            f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
            f"-DPython3_EXECUTABLE={Path(sys.executable).absolute()}",
            "-DGEARS_DYNAREC_CONTRACT_ONLY=ON",
            f"-DX360PORT_ROOT={x360port_root}",
            *dependency_options,
        ],
        cwd=ROOT,
        environ=cmake_child_environment(c_compiler, cxx_compiler),
    )
    runner.run(
        [
            cmake,
            "--build",
            output,
            "--target",
            "test_gears1_dynarec_boundary",
            "--parallel",
            str(selected.parallel),
        ],
        cwd=ROOT,
    )
    runner.run(
        [require_program("ctest"), "--test-dir", output, "--output-on-failure"],
        cwd=ROOT,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
