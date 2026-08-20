#!/usr/bin/env bash
# Non-mutating C++ formatting and lint gate for the runtime control plane.
set -euo pipefail
cd "$(dirname "$0")/.."

build_dir=${1:-${GEARS_BUILD_DIR:-scratch/build}}
format=${CLANG_FORMAT:-$(command -v clang-format || true)}
tidy=${CLANG_TIDY:-$(command -v clang-tidy || true)}

[[ -n $format ]] || { echo "REFUSING: clang-format is not installed" >&2; exit 1; }
[[ -n $tidy ]] || { echo "REFUSING: clang-tidy is not installed" >&2; exit 1; }
[[ -f $build_dir/compile_commands.json ]] || {
    echo "REFUSING: $build_dir/compile_commands.json is missing; configure with" >&2
    echo "  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
    exit 1
}

formatted=(
    runtime/debug_http.cpp runtime/debug_http.h
    runtime/gpu_draw.cpp runtime/gpu_draw_renderer.h runtime/gpu_draw_vertexfetch.cpp
    runtime/gpu_present.cpp runtime/gpu_present_stage.cpp runtime/gpu_present_stage.h
    runtime/gpu_scanout.cpp runtime/gpu_scanout.h
    runtime/gpu_scanout_gamma.cpp runtime/gpu_scanout_gamma.h
    runtime/graphics_probe.cpp runtime/graphics_probe.h
    runtime/graphics_probe_render.cpp runtime/graphics_probe_render.h
    runtime/input.cpp runtime/input.h runtime/render_thread.cpp
    runtime/scanout_gamma.cpp runtime/scanout_gamma.h runtime/swapchain_format.h
    tests/test_graphics_probe.cpp tests/test_remote_input.cpp
    tests/test_scanout_gamma.cpp tests/test_swapchain_format.cpp
)
"$format" --dry-run --Werror "${formatted[@]}"
# This untouched legacy monolith predates the tracked style; check its changed
# regions without pretending the rest is formatted.
"$format" --dry-run --Werror \
    -lines=1:4 -lines=32:34 -lines=1948:1956 \
    -lines=2034:2034 -lines=2970:2974 runtime/vd_null_gpu.cpp

resource_dir=$(clang++ -print-resource-dir)
"$tidy" -p "$build_dir" \
    runtime/debug_http.cpp runtime/graphics_probe.cpp runtime/graphics_probe_render.cpp \
    runtime/gpu_draw.cpp runtime/gpu_draw_vertexfetch.cpp \
    runtime/gpu_present.cpp runtime/gpu_present_stage.cpp \
    runtime/gpu_scanout.cpp runtime/gpu_scanout_gamma.cpp runtime/input.cpp \
    runtime/render_thread.cpp \
    runtime/scanout_gamma.cpp tests/test_graphics_probe.cpp tests/test_remote_input.cpp \
    tests/test_scanout_gamma.cpp tests/test_swapchain_format.cpp \
    --extra-arg="-resource-dir=$resource_dir" --quiet

# Analyze the real legacy translation units while reporting the regions this
# milestone changed. Their pre-existing whole-file diagnostics are separate
# refactoring debt and must not obscure regressions in the new integration.
legacy_filter='[{"name":"runtime/vd_null_gpu.cpp","lines":[[1,4],[32,34],[1948,1956],[2034,2034],[2970,2974]]}]'
"$tidy" -p "$build_dir" runtime/vd_null_gpu.cpp \
    -line-filter="$legacy_filter" \
    --extra-arg="-resource-dir=$resource_dir" --quiet
