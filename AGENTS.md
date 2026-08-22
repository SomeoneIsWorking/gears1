# Gears 1 port guidance

The repository-wide rules in `../../AGENTS.md` apply here. Consult
`docs/codemap.md` before changing a subsystem and update it in the same commit.

## Product target

USER 2026-08-22: "The goal is to build a native UE3 engine using UE3 sources so the game runs well"

The shipping product is a native UE3 runtime built from the available UE3
source and adapted to Gears of War's version-374 Xenon-cooked content. The
recompiled PPC runtime, PM4 command processor, Xenos shader translator, and
guest-draw renderer are the reference oracle and transition path; improving
them is justified when it protects correctness or produces evidence needed by
the native runtime, but they are not the product architecture.

“Native” means the UE3 object/package system, engine loop, scene, gameplay
module, renderer, audio, and input execute as host code. Replacing individual
Xenos shaders or removing one console GPU behavior inside a PM4-driven renderer
does not satisfy this target. Keep the licensed UE3 checkout external through
`RETIRED_PRIVATE_SOURCE_INPUT`; never vendor it or game content into this repository.

## Verification runs

USER 2026-08-22: "don't do windowed runs please always run headless"

Every run started by an agent for verification, profiling, capture, or
diagnosis must use `./run.sh --headless` or a purpose-built headless tool. Do
not open a game window for a smoke test, including when checking the launcher;
exercise the launcher with its headless option instead.

## Host architecture

Use `${DUSKLIGHT_REPO}` when set, otherwise the sibling checkout at
`../../dusklight`, as the ownership reference. Adapt its cohesive host
subsystems rather than copying platform-specific implementations.

- `native/ue3/CMakeLists.txt` owns composition of the external UE3 source build;
  `native/ue3/platform/` owns the host Linux contract, with diagnostics kept in
  `LinuxDiagnostics.*`, filesystem semantics in `HostFileSystem.*`, and the
  UE3 archive adapter in `LinuxFileManager.*` rather than accumulated in
  `Linux.h`. The selected and deferred Core manifests must account for every
  source in Core's current `.vcxproj`. Licensed source seams are exact-checked
  and generated only in the ignored build tree by `tools/prepare_ue3_core.py`;
  never modify the external checkout in place or commit a generated overlay.
- `runtime/vd_null_gpu.cpp` composes guest GPU dispatch with host subsystems; it
  must not absorb their implementations.
- `runtime/input.cpp` owns controller sources, arbitration, and the guest-facing
  controller snapshot.
- Lucent owns the reusable loopback HTTP transport. `runtime/debug_http.cpp`
  owns only Gears routes and translates requests through narrow input/probe
  interfaces.
- `runtime/graphics_probe.cpp` owns on-demand readback state and publication;
  `runtime/graphics_probe_render.cpp` is the narrow renderer adapter. Shipping
  render passes do not depend on HTTP.
- `runtime/frame_probe_capture.h` owns the diagnostic-frame re-arm and the
  report/probe distinction. A probe may bypass a held capture selector, but it
  must not open that selector, consume its quota, or emit its artifacts.
- `runtime/gpu_scanout.cpp` owns finished-frame staging and scan-out transforms;
  `runtime/gpu_scanout_gamma.cpp` owns the Vulkan guest-LUT pass, while
  `runtime/scanout_gamma.cpp` owns the shared pure LUT conversion.
- `runtime/gpu_present.cpp` orchestrates presentation. Swapchain-dependent
  staging resources belong in focused owners such as `gpu_present_stage.cpp`,
  not in the presenter orchestration file.

Pure state transitions belong behind production interfaces with focused tests.
Run `tools/check_source_structure.py`, its self-test, and the CTest suite before
landing host changes; new sources are capped mechanically and legacy monoliths
may not grow.
