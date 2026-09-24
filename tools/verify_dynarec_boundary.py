#!/usr/bin/env python3
"""Build and execute the asset-free Gears/x360port discriminator."""

from __future__ import annotations

import argparse
import platform
import sys
import time
from pathlib import Path

from gearsue3_bootstrap.crash_triage import TriageError, host_backtrace, report_backtraces
from gearsue3_bootstrap.paths import build_directory
from gearsue3_bootstrap.process import CommandError, CommandRunner

ROOT = Path(__file__).resolve().parents[1]


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--x360port-root", required=True, type=Path)
    parser.add_argument(
        "--x360ue3-root", default=ROOT / "../../shared/x360ue3", type=Path
    )
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
    x360ue3_root = selected.x360ue3_root.resolve()
    if not (x360ue3_root / "CMakeLists.txt").is_file():
        raise ValueError(f"x360ue3 root is not a source checkout: {x360ue3_root}")
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
            f"-DX360UE3_ROOT={x360ue3_root}",
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
            # CMake owns the binary CTest set; building the aggregate target
            # keeps the built binaries and the registered tests in step.
            "gears1_ctest_binaries",
            "--parallel",
            str(selected.parallel),
        ],
        cwd=ROOT,
    )
    ctest = require_program("ctest")
    junit_report = output / "ctest-results.xml"
    # Crash reports written from here on belong to this run's tests.
    tests_began = time.time()
    try:
        runner.run(
            [ctest, "--test-dir", output, "--output-on-failure",
             "--output-junit", junit_report],
            cwd=ROOT,
        )
    except CommandError:
        # A test that died on a signal may have printed nothing; show where.
        try:
            report_backtraces(
                junit_report,
                runner.capture([ctest, "--test-dir", output, "--show-only=json-v1"], cwd=ROOT),
                host_backtrace(platform.system(), tests_began))
        except TriageError as error:
            print(f"crash triage: {error}", file=sys.stderr)
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
