#!/usr/bin/env python3
"""Non-mutating clang-format and clang-tidy gate for first-party C++."""

from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
CPP_ROOTS = ("runtime", "tests", "tools", "xenia_gpu")
EXCLUDED_PARTS = {"build", "extern", "scratch", ".git", ".venv", "__pycache__"}
MAINTAINED_FILES = tuple(
    Path(path)
    for path in (
        "runtime/byte_order.h",
        "runtime/crt_printf.cpp",
        "runtime/engine/bsp/bsp_geometry.cpp",
        "runtime/engine/bsp/bsp_geometry.h",
        "runtime/engine/bsp/bsp_model.cpp",
        "runtime/engine/bsp/bsp_model.h",
        "runtime/engine/bsp/component_geometry.cpp",
        "runtime/engine/bsp/component_geometry.h",
        "runtime/engine/bsp/model_component.cpp",
        "runtime/engine/bsp/model_component.h",
        "runtime/engine/material/material_textures.cpp",
        "runtime/engine/material/material_textures.h",
        "runtime/engine/mesh/static_mesh.cpp",
        "runtime/engine/mesh/static_mesh.h",
        "runtime/engine/object/bulk_data.cpp",
        "runtime/engine/object/bulk_data.h",
        "runtime/engine/object/class_hierarchy.cpp",
        "runtime/engine/object/class_hierarchy.h",
        "runtime/engine/object/object_resolver.cpp",
        "runtime/engine/object/object_resolver.h",
        "runtime/engine/object/property_values.cpp",
        "runtime/engine/object/property_values.h",
        "runtime/engine/object/serialized_object.cpp",
        "runtime/engine/object/serialized_object.h",
        "runtime/engine/object/tagged_properties.cpp",
        "runtime/engine/object/tagged_properties.h",
        "runtime/engine/package/byte_reader.cpp",
        "runtime/engine/package/byte_reader.h",
        "runtime/engine/package/compressed_record.cpp",
        "runtime/engine/package/compressed_record.h",
        "runtime/engine/package/content_files.cpp",
        "runtime/engine/package/content_files.h",
        "runtime/engine/package/lzo1x.cpp",
        "runtime/engine/package/lzo1x.h",
        "runtime/engine/package/object_tables.cpp",
        "runtime/engine/package/object_tables.h",
        "runtime/engine/package/package.cpp",
        "runtime/engine/package/package.h",
        "runtime/engine/package/package_constants.h",
        "runtime/engine/package/package_store.cpp",
        "runtime/engine/package/package_store.h",
        "runtime/engine/package/package_summary.cpp",
        "runtime/engine/package/package_summary.h",
        "runtime/engine/render/device_image.cpp",
        "runtime/engine/render/device_image.h",
        "runtime/engine/render/gpu_texture.cpp",
        "runtime/engine/render/gpu_texture.h",
        "runtime/engine/render/level_renderer.cpp",
        "runtime/engine/render/level_renderer.h",
        "runtime/engine/render/mesh_renderer.cpp",
        "runtime/engine/render/mesh_renderer.h",
        "runtime/engine/render/offscreen_target.cpp",
        "runtime/engine/render/offscreen_target.h",
        "runtime/engine/render/texture_bindings.cpp",
        "runtime/engine/render/texture_bindings.h",
        "runtime/engine/render/vulkan_device.cpp",
        "runtime/engine/render/vulkan_device.h",
        "runtime/engine/scene/camera.cpp",
        "runtime/engine/scene/camera.h",
        "runtime/engine/scene/level_scene.cpp",
        "runtime/engine/scene/level_scene.h",
        "runtime/engine/scene/transform.cpp",
        "runtime/engine/scene/transform.h",
        "runtime/engine/texture/pixel_format.h",
        "runtime/engine/texture/texture2d.cpp",
        "runtime/engine/texture/texture2d.h",
        "runtime/engine/texture/xenos_tiling.cpp",
        "runtime/engine/texture/xenos_tiling.h",
        "runtime/fault_report.cpp",
        "runtime/fault_report.h",
        "runtime/frame_capture.h",
        "runtime/gears1_guest_image.cpp",
        "runtime/gears1_guest_image.h",
        "runtime/gpu_packet_memory.cpp",
        "runtime/guest_backtrace.cpp",
        "runtime/guest_backtrace.h",
        "runtime/guest_clock.cpp",
        "runtime/guest_dirty_pages.h",
        "runtime/guest_memory.cpp",
        "runtime/guest_memory.h",
        "runtime/guest_texture_hash.cpp",
        "runtime/guest_thread.cpp",
        "runtime/host_time_zone.h",
        "runtime/input.cpp",
        "runtime/input.h",
        "runtime/kernel_config.cpp",
        "runtime/kernel_dispatcher.cpp",
        "runtime/kernel_events.cpp",
        "runtime/kernel_file.cpp",
        "runtime/kernel_memory.cpp",
        "runtime/kernel_misc.cpp",
        "runtime/kernel_object_api.cpp",
        "runtime/kernel_objects.cpp",
        "runtime/kernel_rtl.cpp",
        "runtime/kernel_spinlock.cpp",
        "runtime/kernel_sync.cpp",
        "runtime/kernel_thread.cpp",
        "runtime/kernel_time.cpp",
        "runtime/kernel_timer.cpp",
        "runtime/kernel_video.cpp",
        "runtime/missing_x360port_executor.h",
        "runtime/pm4_trace.cpp",
        "runtime/product/control_channel.cpp",
        "runtime/product/control_channel.h",
        "runtime/product/gears1_main.cpp",
        "runtime/product/gears1_session.cpp",
        "runtime/product/gears1_session.h",
        "runtime/product/memory_request.cpp",
        "runtime/product/memory_request.h",
        "runtime/product/offscreen_run.cpp",
        "runtime/product/offscreen_run.h",
        "runtime/product/pad_form.cpp",
        "runtime/product/pad_form.h",
        "runtime/product/portable_pixmap.cpp",
        "runtime/product/portable_pixmap.h",
        "runtime/product/product_options.cpp",
        "runtime/product/product_options.h",
        "runtime/product/run_stop.h",
        "runtime/title_profile.cpp",
        "runtime/title_profile.h",
        "runtime/titles/gears1/audio_mix.cpp",
        "runtime/titles/gears1/audio_mix.h",
        "runtime/titles/gears1/audio_mix_differential.cpp",
        "runtime/titles/gears1/audio_mix_differential.h",
        "runtime/titles/gears1/desktop_controls.cpp",
        "runtime/titles/gears1/desktop_controls.h",
        "runtime/titles/gears1/guest_chain.cpp",
        "runtime/titles/gears1/guest_chain.h",
        "runtime/titles/gears1/navigation_probe.cpp",
        "runtime/titles/gears1/navigation_probe.h",
        "runtime/titles/gears1/player_probe.cpp",
        "runtime/titles/gears1/player_probe.h",
        "runtime/titles/gears1/presentation.h",
        "runtime/titles/gears1/xam_input_provider.cpp",
        "runtime/titles/gears1/xam_input_provider.h",
        "runtime/titles/gears1/xam_video_services.cpp",
        "runtime/titles/gears1/xam_video_services.h",
        "runtime/vd_null_gpu.cpp",
        "runtime/wait_probe.cpp",
        "runtime/wait_probe.h",
        "runtime/xam_loader.cpp",
        "runtime/xam_notify.cpp",
        "runtime/xam_overlapped.cpp",
        "runtime/xam_user.cpp",
        "runtime/xaudio_null.cpp",
        "runtime/xconfig.cpp",
        "runtime/xma.cpp",
        "runtime/xma.h",
        "runtime/xma_context.cpp",
        "runtime/xnet_null.cpp",
        "tests/engine_package_fixture.h",
        "tests/fake_guest_memory.h",
        "tests/test_audio_mix_differential.cpp",
        "tests/test_engine_bsp.cpp",
        "tests/test_engine_object.cpp",
        "tests/test_engine_package.cpp",
        "tests/test_engine_scene.cpp",
        "tests/test_gears1_desktop_controls.cpp",
        "tests/test_gears1_dynarec_boundary.cpp",
        "tests/test_gears1_product_options.cpp",
        "tests/test_gears1_real_leaf.cpp",
        "tests/test_memory_request.cpp",
        "tests/test_navigation_probe.cpp",
        "tests/test_pad_form.cpp",
        "tests/test_player_probe.cpp",
        "tests/test_remote_input.cpp",
        "tests/test_script_handoff.cpp",
        "tests/test_title_profile.cpp",
        "tools/heap_replay.cpp",
        "tools/level_render/main.cpp",
        "tools/package_census/main.cpp",
        "tools/package_inspect/main.cpp",
        "tools/system_constants/main.cpp",
        "tools/texture_export/main.cpp",
        "tools/xenos_translate/main.cpp",
        "tools/xma_replay.cpp",
        "xenia_gpu/xenia_host_shim.cpp",
    )
)


def find_tool(name: str, override: str | None = None, finder=shutil.which) -> str:
    candidate = override or finder(name)
    if not candidate:
        raise RuntimeError(f"{name} is not installed")
    return candidate


def is_generated_source(path: Path) -> bool:
    try:
        prefix = path.read_text(encoding="utf-8")[:512]
    except (OSError, UnicodeDecodeError):
        return False
    return (
        "GENERATED from " in prefix or "generated file -- do not edit" in prefix.lower()
    )


def first_party_cpp(root: Path) -> list[Path]:
    files: list[Path] = []
    for source_root in CPP_ROOTS:
        directory = root / source_root
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            relative = path.relative_to(root)
            if any(part in EXCLUDED_PARTS for part in relative.parts):
                continue
            if (
                path.is_file()
                and path.suffix.lower() in CPP_SUFFIXES
                and not is_generated_source(path)
            ):
                files.append(relative)
    return sorted(files)


def compile_database_sources(build_dir: Path) -> set[Path]:
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise RuntimeError(
            f"{database} is missing; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        )
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
        sources: set[Path] = set()
        for entry in entries:
            source = Path(entry["file"])
            if not source.is_absolute():
                source = Path(entry["directory"]) / source
            sources.add(source.resolve())
        return sources
    except (OSError, KeyError, TypeError, ValueError) as error:
        raise RuntimeError(f"{database} is invalid: {error}") from error


def selected_tidy_units(root: Path, database_sources: set[Path]) -> list[Path]:
    units = [
        path
        for path in MAINTAINED_FILES
        if path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}
        and (root / path).resolve() in database_sources
    ]
    if not units:
        raise RuntimeError("compile database contains no first-party translation units")
    return units


def sdk_arguments(system: str, sdk_path: Callable[[], str]) -> list[str]:
    """clang-tidy arguments naming the SDK a compiler driver finds implicitly.

    On macOS, AppleClang finds the SDK itself and CMake 4 no longer writes
    -isysroot into the compile database, so another clang driver such as
    Homebrew's clang-tidy would find no standard headers without it.
    """

    if system != "Darwin":
        return []
    path = sdk_path()
    if not path:
        raise RuntimeError("xcrun named no macOS SDK")
    return [f"--extra-arg=-isysroot{path}"]


def xcrun_sdk_path() -> str:
    return subprocess.run(
        ["xcrun", "--show-sdk-path"], check=True, text=True, capture_output=True
    ).stdout.strip()


def run(command: list[str], root: Path) -> None:
    subprocess.run(command, cwd=root, check=True)


def selftest() -> int:
    fake = lambda name: f"/tools/{name}" if name != "missing" else None
    assert find_tool("clang-format", finder=fake) == "/tools/clang-format"
    try:
        find_tool("missing", finder=fake)
    except RuntimeError:
        pass
    else:
        raise AssertionError("missing tools must be refused")
    assert sdk_arguments("Linux", lambda: "/sdk") == []
    assert sdk_arguments("Darwin", lambda: "/sdk") == ["--extra-arg=-isysroot/sdk"]
    try:
        sdk_arguments("Darwin", lambda: "")
    except RuntimeError:
        pass
    else:
        raise AssertionError("a macOS host without an SDK must be refused")

    root = Path(__file__).resolve().parents[1]
    discovered = first_party_cpp(root)
    assert Path("runtime/byte_order.h") in discovered
    assert Path("runtime/wait_probe.cpp") in discovered
    assert Path("tools/heap_replay.cpp") in discovered
    assert all((root / path).is_file() for path in MAINTAINED_FILES)
    assert not any(is_generated_source(root / path) for path in MAINTAINED_FILES)
    selected = selected_tidy_units(root, {(root / "runtime/wait_probe.cpp").resolve()})
    assert selected == [Path("runtime/wait_probe.cpp")]
    print(
        f"C++ quality checker selftest passed: discovered {len(discovered)} current "
        f"first-party files and validated {len(MAINTAINED_FILES)} maintained files; "
        "missing-tool, missing-SDK and compiled-unit refusals exercised"
    )
    return 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--selftest"]:
        return selftest()
    if len(argv) > 2:
        print(f"usage: {argv[0]} [build-dir]", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parents[1]
    build_dir = (
        Path(argv[1])
        if len(argv) == 2
        else Path(os.environ.get("GEARS_BUILD_DIR", "build/release"))
    )
    if not build_dir.is_absolute():
        build_dir = root / build_dir

    try:
        clang_format = find_tool("clang-format", os.environ.get("CLANG_FORMAT"))
        clang_tidy = find_tool("clang-tidy", os.environ.get("CLANG_TIDY"))
        clang_cxx = find_tool("clang++", os.environ.get("CLANG_CXX"))
        formatted = list(MAINTAINED_FILES)
        if not formatted:
            raise RuntimeError("no first-party C++ files discovered")
        missing = [path for path in formatted if not (root / path).is_file()]
        if missing:
            raise RuntimeError(
                "maintained C++ manifest contains missing files: "
                + ", ".join(map(str, missing))
            )
        database_sources = compile_database_sources(build_dir)
        tidy_units = selected_tidy_units(root, database_sources)
        sdk = sdk_arguments(platform.system(), xcrun_sdk_path)
    except RuntimeError as error:
        print(f"REFUSING: {error}", file=sys.stderr)
        return 1

    run([clang_format, "--dry-run", "--Werror", *map(str, formatted)], root)
    resource_dir = subprocess.run(
        [clang_cxx, "-print-resource-dir"], check=True, text=True, capture_output=True
    ).stdout.strip()
    run(
        [
            clang_tidy,
            "-p",
            str(build_dir),
            f"--extra-arg=-resource-dir={resource_dir}",
            *sdk,
            "--quiet",
            *map(str, tidy_units),
        ],
        root,
    )
    print(
        f"C++ quality gate passed: formatted {len(formatted)} first-party files; "
        f"linted {len(tidy_units)} compiled first-party units"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
