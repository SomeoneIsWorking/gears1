#!/usr/bin/env python3
"""Non-mutating clang-format and clang-tidy gate for first-party C++."""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


FORMATTED = [
    "runtime/generated_title_profile.cpp",
    "runtime/generated_title_profile.h",
    "runtime/guest_filesystem.cpp",
    "runtime/guest_filesystem.h",
    "runtime/guest_dirty_pages.cpp",
    "runtime/guest_dirty_pages.h",
    "runtime/guest_texture_hash.cpp",
    "runtime/guest_texture_hash.h",
    "runtime/guest_write_watch.cpp",
    "runtime/guest_write_watch.h",
    "runtime/main.cpp",
    "runtime/debug_http.cpp",
    "runtime/debug_http.h",
    "runtime/frame_probe_capture.h",
    "runtime/frame_contract.cpp",
    "runtime/frame_contract.h",
    "runtime/frame_queue.cpp",
    "runtime/frame_queue.h",
    "runtime/gpu_device_features.h",
    "runtime/gpu_draw.cpp",
    "runtime/gpu_draw_ab.cpp",
    "runtime/gpu_draw_ab.h",
    "runtime/gpu_draw.h",
    "runtime/gpu_draw_api.cpp",
    "runtime/gpu_draw_arena.cpp",
    "runtime/gpu_draw_arena.h",
    "runtime/gpu_draw_indices.cpp",
    "runtime/gpu_draw_indices.h",
    "runtime/gpu_draw_options.cpp",
    "runtime/gpu_draw_options.h",
    "runtime/gpu_draw_pixels.cpp",
    "runtime/gpu_draw_pixels.h",
    "runtime/gpu_draw_pipelines.cpp",
    "runtime/gpu_draw_pipelines.h",
    "runtime/gpu_draw_point_geometry.cpp",
    "runtime/gpu_draw_prepared.h",
    "runtime/gpu_draw_probe.h",
    "runtime/gpu_draw_reinterpret.cpp",
    "runtime/gpu_draw_renderer.h",
    "runtime/gpu_draw_resolve.cpp",
    "runtime/gpu_draw_resolve_decode.cpp",
    "runtime/gpu_draw_resolve_decode.h",
    "runtime/gpu_draw_sample_layout.h",
    "runtime/gpu_draw_shaders.cpp",
    "runtime/gpu_draw_targets.cpp",
    "runtime/gpu_draw_targets.h",
    "runtime/gpu_draw_textures.cpp",
    "runtime/gpu_draw_textures.h",
    "runtime/gpu_draw_uniforms.cpp",
    "runtime/gpu_draw_uniforms.h",
    "runtime/gpu_draw_vertexfetch.cpp",
    "runtime/gpu_draw_xlate.cpp",
    "runtime/gpu_draw_xlate.h",
    "runtime/gpu_endian.h",
    "runtime/gpu_surface_format_capacity.h",
    "runtime/gpu_frame_capacity.h",
    "runtime/gpu_frame_cleanup.cpp",
    "runtime/gpu_frame_cleanup.h",
    "runtime/gpu_frame_slots.cpp",
    "runtime/gpu_frame_slots.h",
    "runtime/gpu_frame_timing.cpp",
    "runtime/gpu_frame_timing.h",
    "runtime/gpu_present.cpp",
    "runtime/gpu_present_source.cpp",
    "runtime/gpu_present_source.h",
    "runtime/gpu_present_stage.cpp",
    "runtime/gpu_present_stage.h",
    "runtime/gpu_queue_access.cpp",
    "runtime/gpu_queue_access.h",
    "runtime/gpu_register_watch.cpp",
    "runtime/gpu_register_watch.h",
    "runtime/gpu_retirement.cpp",
    "runtime/gpu_retirement.h",
    "runtime/gpu_renderer_capacity.cpp",
    "runtime/gpu_packet_memory.cpp",
    "runtime/gpu_packet_memory.h",
    "runtime/gpu_scanout.cpp",
    "runtime/gpu_scanout.h",
    "runtime/gpu_scanout_gamma.cpp",
    "runtime/gpu_scanout_gamma.h",
    "runtime/gpu_shared_device.cpp",
    "runtime/gpu_shared_device.h",
    "runtime/gpu_swap_packet.cpp",
    "runtime/gpu_swap_packet.h",
    "runtime/gpu_renderer_lifetime.cpp",
    "runtime/graphics_probe.cpp",
    "runtime/graphics_probe.h",
    "runtime/graphics_probe_render.cpp",
    "runtime/graphics_probe_render.h",
    "runtime/input.cpp",
    "runtime/input.h",
    "runtime/native_pass.cpp",
    "runtime/native_pass.h",
    "runtime/render_thread.cpp",
    "runtime/render_thread.h",
    "runtime/render_retirement.h",
    "runtime/rhi_semantic_state.cpp",
    "runtime/rhi_semantic_state.h",
    "runtime/rhi_semantic_stream.cpp",
    "runtime/rhi_semantic_stream.h",
    "runtime/scanout_gamma.cpp",
    "runtime/scanout_gamma.h",
    "runtime/title_executable.cpp",
    "runtime/title_executable.h",
    "runtime/title_profile.cpp",
    "runtime/title_profile.h",
    "runtime/titles/gears1/rhi_bindings.cpp",
    "runtime/titles/gears1/rhi_index_buffer.cpp",
    "runtime/titles/gears1/rhi_index_buffer.h",
    "runtime/titles/gears1/rhi_vertex_buffer.cpp",
    "runtime/titles/gears1/rhi_vertex_buffer.h",
    "runtime/titles/gears1/shader_setter_override.cpp",
    "runtime/titles/gears1/shader_setter_state.h",
    "runtime/vd_null_gpu.cpp",
    "runtime/swapchain_format.h",
    "tests/test_depth_alias_shader_format.cpp",
    "tests/test_frame_probe_capture.cpp",
    "tests/test_frame_contract.cpp",
    "tests/test_frame_queue.cpp",
    "tests/test_graphics_probe.cpp",
    "tests/test_guest_dirty_pages.cpp",
    "tests/test_guest_texture_hash.cpp",
    "tests/test_guest_write_watch.cpp",
    "tests/test_gpu_draw_sample_layout.cpp",
    "tests/test_gpu_draw_untile.cpp",
    "tests/test_gpu_draw_options.cpp",
    "tests/test_gpu_draw_indices.cpp",
    "tests/test_gpu_retirement.cpp",
    "tests/test_gpu_frame_timing.cpp",
    "tests/test_gpu_queue_access.cpp",
    "tests/test_gpu_draw_census.cpp",
    "tests/test_shader_setter_state.cpp",
    "tests/test_gpu_surface_format_capacity.cpp",
    "tests/test_gpu_surface_target_lookup.cpp",
    "tests/test_gpu_swap_packet.cpp",
    "tests/test_remote_input.cpp",
    "tests/test_rhi_index_buffer.cpp",
    "tests/test_rhi_semantic_state.cpp",
    "tests/test_rhi_vertex_buffer.cpp",
    "tests/test_rhi_semantic_stream.cpp",
    "tests/test_render_retirement.cpp",
    "tests/test_shared_frame_image.cpp",
    "tests/test_scanout_gamma.cpp",
    "tests/test_spirv_clamp.cpp",
    "tests/test_swapchain_format.cpp",
    "tests/test_generated_title_profile.cpp",
    "tests/test_title_executable.cpp",
    "tests/test_title_profile.cpp",
]

TIDY_TRANSLATION_UNITS = [
    "runtime/generated_title_profile.cpp",
    "runtime/guest_filesystem.cpp",
    "runtime/guest_texture_hash.cpp",
    "runtime/guest_write_watch.cpp",
    "runtime/main.cpp",
    "runtime/debug_http.cpp",
    "runtime/graphics_probe.cpp",
    "runtime/graphics_probe_render.cpp",
    "runtime/frame_contract.cpp",
    "runtime/frame_queue.cpp",
    "runtime/gpu_draw.cpp",
    "runtime/gpu_draw_ab.cpp",
    "runtime/gpu_draw_api.cpp",
    "runtime/gpu_draw_arena.cpp",
    "runtime/gpu_draw_indices.cpp",
    "runtime/gpu_draw_options.cpp",
    "runtime/gpu_draw_pixels.cpp",
    "runtime/gpu_draw_pipelines.cpp",
    "runtime/gpu_draw_point_geometry.cpp",
    "runtime/gpu_register_watch.cpp",
    "runtime/gpu_draw_reinterpret.cpp",
    "runtime/gpu_draw_resolve.cpp",
    "runtime/gpu_draw_resolve_decode.cpp",
    "runtime/gpu_draw_shaders.cpp",
    "runtime/gpu_draw_targets.cpp",
    "runtime/gpu_draw_textures.cpp",
    "runtime/gpu_draw_uniforms.cpp",
    "runtime/gpu_draw_vertexfetch.cpp",
    "runtime/gpu_draw_xlate.cpp",
    "runtime/gpu_frame_cleanup.cpp",
    "runtime/gpu_frame_slots.cpp",
    "runtime/gpu_frame_timing.cpp",
    "runtime/gpu_present.cpp",
    "runtime/gpu_present_source.cpp",
    "runtime/gpu_present_stage.cpp",
    "runtime/gpu_queue_access.cpp",
    "runtime/gpu_retirement.cpp",
    "runtime/gpu_renderer_capacity.cpp",
    "runtime/gpu_packet_memory.cpp",
    "runtime/gpu_scanout.cpp",
    "runtime/gpu_scanout_gamma.cpp",
    "runtime/gpu_shared_device.cpp",
    "runtime/gpu_swap_packet.cpp",
    "runtime/gpu_renderer_lifetime.cpp",
    "runtime/input.cpp",
    "runtime/native_pass.cpp",
    "runtime/render_thread.cpp",
    "runtime/rhi_semantic_state.cpp",
    "runtime/rhi_semantic_stream.cpp",
    "runtime/scanout_gamma.cpp",
    "runtime/title_executable.cpp",
    "runtime/title_profile.cpp",
    "runtime/titles/gears1/rhi_bindings.cpp",
    "runtime/titles/gears1/rhi_index_buffer.cpp",
    "runtime/titles/gears1/rhi_vertex_buffer.cpp",
    "runtime/titles/gears1/shader_setter_override.cpp",
    "tests/test_depth_alias_shader_format.cpp",
    "tests/test_frame_probe_capture.cpp",
    "tests/test_frame_contract.cpp",
    "tests/test_frame_queue.cpp",
    "tests/test_graphics_probe.cpp",
    "tests/test_guest_dirty_pages.cpp",
    "tests/test_guest_texture_hash.cpp",
    "tests/test_guest_write_watch.cpp",
    "tests/test_gpu_draw_sample_layout.cpp",
    "tests/test_gpu_draw_untile.cpp",
    "tests/test_gpu_draw_options.cpp",
    "tests/test_gpu_draw_indices.cpp",
    "tests/test_gpu_retirement.cpp",
    "tests/test_gpu_frame_timing.cpp",
    "tests/test_gpu_queue_access.cpp",
    "tests/test_gpu_draw_census.cpp",
    "tests/test_shader_setter_state.cpp",
    "tests/test_gpu_surface_format_capacity.cpp",
    "tests/test_gpu_surface_target_lookup.cpp",
    "tests/test_gpu_swap_packet.cpp",
    "tests/test_remote_input.cpp",
    "tests/test_rhi_index_buffer.cpp",
    "tests/test_rhi_semantic_state.cpp",
    "tests/test_rhi_vertex_buffer.cpp",
    "tests/test_rhi_semantic_stream.cpp",
    "tests/test_render_retirement.cpp",
    "tests/test_shared_frame_image.cpp",
    "tests/test_scanout_gamma.cpp",
    "tests/test_spirv_clamp.cpp",
    "tests/test_swapchain_format.cpp",
    "tests/test_generated_title_profile.cpp",
    "tests/test_title_executable.cpp",
    "tests/test_title_profile.cpp",
]

VD_TIDY_RANGES = [
    [445, 456],
    [815, 815],
    [1005, 1053],
    [1339, 1339],
    [2621, 2696],
    [3165, 3245],
]


def find_tool(name, override=None, finder=shutil.which):
    candidate = override or finder(name)
    if not candidate:
        raise RuntimeError(f"{name} is not installed")
    return candidate


def compile_database_sources(build_dir):
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise RuntimeError(
            f"{database} is missing; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        )
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
        sources = set()
        for entry in entries:
            source = Path(entry["file"])
            if not source.is_absolute():
                source = Path(entry["directory"]) / source
            sources.add(source.resolve())
        return sources
    except (OSError, KeyError, TypeError, ValueError) as error:
        raise RuntimeError(f"{database} is invalid: {error}") from error


def selected_tidy_units(root, database_sources):
    return TIDY_TRANSLATION_UNITS


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
    assert "runtime/gpu_draw_targets.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_draw_api.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_draw_arena.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_frame_cleanup.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_frame_slots.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_present_source.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_shared_device.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_swap_packet.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_gpu_draw_sample_layout.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_gpu_draw_untile.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_gpu_surface_format_capacity.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_gpu_swap_packet.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_frame_probe_capture.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/frame_queue.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_frame_queue.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_packet_memory.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_queue_access.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/gpu_renderer_capacity.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_gpu_queue_access.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_render_retirement.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_shared_frame_image.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/title_executable.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/generated_title_profile.cpp" in TIDY_TRANSLATION_UNITS
    assert "runtime/guest_texture_hash.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_guest_dirty_pages.cpp" in TIDY_TRANSLATION_UNITS
    assert "tests/test_guest_texture_hash.cpp" in TIDY_TRANSLATION_UNITS
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
        database_sources = compile_database_sources(build_dir)
    except RuntimeError as error:
        print(f"REFUSING: {error}", file=sys.stderr)
        return 1

    run([clang_format, "--dry-run", "--Werror", *FORMATTED], root)

    resource_dir = subprocess.run(
        [clang_cxx, "-print-resource-dir"], check=True, text=True,
        capture_output=True
    ).stdout.strip()
    tidy_common = [
        "-p", str(build_dir), f"--extra-arg=-resource-dir={resource_dir}", "--quiet"
    ]
    tidy_units = selected_tidy_units(root, database_sources)
    run([clang_tidy, *tidy_common, *tidy_units], root)

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
