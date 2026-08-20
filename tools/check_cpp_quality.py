#!/usr/bin/env python3
"""Non-mutating clang-format and clang-tidy gate for first-party C++."""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


FORMATTED = [
    "runtime/debug_http.cpp",
    "runtime/debug_http.h",
    "runtime/frame_probe_capture.h",
    "runtime/gpu_device_features.h",
    "runtime/gpu_draw.cpp",
    "runtime/gpu_draw_pipelines.cpp",
    "runtime/gpu_draw_pipelines.h",
    "runtime/gpu_draw_point_geometry.cpp",
    "runtime/gpu_draw_reinterpret.cpp",
    "runtime/gpu_draw_renderer.h",
    "runtime/gpu_draw_vertexfetch.cpp",
    "runtime/gpu_draw_xlate.cpp",
    "runtime/gpu_draw_xlate.h",
    "runtime/gpu_present.cpp",
    "runtime/gpu_present_stage.cpp",
    "runtime/gpu_present_stage.h",
    "runtime/gpu_scanout.cpp",
    "runtime/gpu_scanout.h",
    "runtime/gpu_scanout_gamma.cpp",
    "runtime/gpu_scanout_gamma.h",
    "runtime/graphics_probe.cpp",
    "runtime/graphics_probe.h",
    "runtime/graphics_probe_render.cpp",
    "runtime/graphics_probe_render.h",
    "runtime/input.cpp",
    "runtime/input.h",
    "runtime/render_thread.cpp",
    "runtime/scanout_gamma.cpp",
    "runtime/scanout_gamma.h",
    "runtime/swapchain_format.h",
    "tests/test_depth_alias_shader_format.cpp",
    "tests/test_frame_probe_capture.cpp",
    "tests/test_graphics_probe.cpp",
    "tests/test_remote_input.cpp",
    "tests/test_scanout_gamma.cpp",
    "tests/test_swapchain_format.cpp",
]

TIDY_TRANSLATION_UNITS = [
    "runtime/debug_http.cpp",
    "runtime/graphics_probe.cpp",
    "runtime/graphics_probe_render.cpp",
    "runtime/gpu_draw.cpp",
    "runtime/gpu_draw_pipelines.cpp",
    "runtime/gpu_draw_point_geometry.cpp",
    "runtime/gpu_draw_reinterpret.cpp",
    "runtime/gpu_draw_vertexfetch.cpp",
    "runtime/gpu_draw_xlate.cpp",
    "runtime/gpu_present.cpp",
    "runtime/gpu_present_stage.cpp",
    "runtime/gpu_scanout.cpp",
    "runtime/gpu_scanout_gamma.cpp",
    "runtime/input.cpp",
    "runtime/render_thread.cpp",
    "runtime/scanout_gamma.cpp",
    "tests/test_depth_alias_shader_format.cpp",
    "tests/test_frame_probe_capture.cpp",
    "tests/test_graphics_probe.cpp",
    "tests/test_remote_input.cpp",
    "tests/test_scanout_gamma.cpp",
    "tests/test_swapchain_format.cpp",
]

VD_FORMAT_RANGES = [
    (35, 35),
    (800, 800),
    (1342, 1342),
    (1474, 1493),
    (1856, 1856),
    (1872, 1879),
    (1922, 1922),
    (1927, 1933),
    (1945, 1945),
    (1956, 1959),
    (1961, 1961),
    (2012, 2022),
    (2065, 2066),
]

VD_TIDY_RANGES = [[first, last] for first, last in VD_FORMAT_RANGES]


def find_tool(name, override=None, finder=shutil.which):
    candidate = override or finder(name)
    if not candidate:
        raise RuntimeError(f"{name} is not installed")
    return candidate


def require_compile_database(build_dir):
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise RuntimeError(
            f"{database} is missing; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        )


def run(command, root):
    subprocess.run(command, cwd=root, check=True)


def selftest():
    fake = lambda name: f"/tools/{name}" if name != "missing" else None
    assert find_tool("clang-format", finder=fake) == "/tools/clang-format"
    try:
        find_tool("missing", finder=fake)
    except RuntimeError:
        pass
    else:
        raise AssertionError("missing tools must be refused")
    assert "runtime/gpu_draw_reinterpret.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_frame_probe_capture.cpp" in TIDY_TRANSLATION_UNITS
    assert VD_TIDY_RANGES and all(first <= last for first, last in VD_TIDY_RANGES)
    print("C++ quality checker selftest passed: positive tool lookup, missing-tool refusal, "
          "and touched-source coverage")
    return 0


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    if len(argv) > 2:
        print(f"usage: {argv[0]} [build-dir]", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parents[1]
    build_dir = Path(argv[1]) if len(argv) == 2 else Path(
        os.environ.get("GEARS_BUILD_DIR", "scratch/build")
    )
    if not build_dir.is_absolute():
        build_dir = root / build_dir

    try:
        clang_format = find_tool("clang-format", os.environ.get("CLANG_FORMAT"))
        clang_tidy = find_tool("clang-tidy", os.environ.get("CLANG_TIDY"))
        clang_cxx = find_tool("clang++", os.environ.get("CLANG_CXX"))
        require_compile_database(build_dir)
    except RuntimeError as error:
        print(f"REFUSING: {error}", file=sys.stderr)
        return 1

    run([clang_format, "--dry-run", "--Werror", *FORMATTED], root)
    vd_format = [clang_format, "--dry-run", "--Werror"]
    vd_format.extend(
        f"-lines={first}:{last}" for first, last in VD_FORMAT_RANGES
    )
    vd_format.append("runtime/vd_null_gpu.cpp")
    run(vd_format, root)

    resource_dir = subprocess.run(
        [clang_cxx, "-print-resource-dir"], check=True, text=True,
        capture_output=True
    ).stdout.strip()
    tidy_common = [
        "-p", str(build_dir), f"--extra-arg=-resource-dir={resource_dir}", "--quiet"
    ]
    run([clang_tidy, *tidy_common, *TIDY_TRANSLATION_UNITS], root)

    line_filter = json.dumps([
        {"name": "runtime/vd_null_gpu.cpp", "lines": VD_TIDY_RANGES}
    ], separators=(",", ":"))
    run([
        clang_tidy, "-p", str(build_dir), "runtime/vd_null_gpu.cpp",
        f"-line-filter={line_filter}", f"--extra-arg=-resource-dir={resource_dir}",
        "--quiet",
    ], root)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
