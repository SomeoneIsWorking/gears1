# GearsUE3 engine-port guidance

The repository-wide rules in `../../AGENTS.md` apply here. Consult
`docs/codemap.md` before changing a subsystem and update it in the same commit.

## Product target

USER 2026-08-22: "I mean we can continue optimizing the recomp path but make it like an engine port so like GearsUE3 engine with recomp + overrides but it is supposed to handle all Gears of War UE3 games not just the first one"

The product is GearsUE3: a shared engine port built from static recompilation,
host Xbox 360 services, and measured native overrides. Gears 1 is the first
conformance target, not the ownership boundary. Each supported title/revision
gets its own locally generated recomp module and factual binding profile while
the host runtime, renderer, audio, input, storage, override infrastructure, and
semantic native implementations remain shared.

Keep every recompiled body compiled and callable. Native overrides are runtime
A/B seams with an explicit super-call, not edits to generated output or
compile-time deletions. Guest addresses, image identity, shader hashes, menu
walks, and diagnostics belong to a title/revision adapter; shared engine code
must not acquire Gears 1 address tables merely because it is the first target.

## Clean distribution boundary

USER 2026-08-22: "I don't want to provide copyrighted material in anyway either from a private repo or a download link, I only want to provide absolute clean code and others should only provide the ROMs"

The public repository contains independently authored source, compatible
open-source dependencies with their required notices, and factual interoperability
metadata only. It must not contain or fetch UE3 source, game code/assets,
extracted files, decoded shader listings, decompiler output, generated recomp
bodies, or caches derived from a title. Do not instruct users to obtain private
source or game files from a download. A user-owned disc/image is the only
copyrighted input accepted by provisioning.

All disc-derived output belongs under ignored `scratch/titles/<fingerprint>/`
and must be regenerable. Private UE3 source may settle a conceptual question for
a developer, but no expression, comment, declaration, patch context, mechanical
translation, or generated artifact from it may enter this repository. Public
implementations must be independently written and verified against observable
behavior or locally supplied game bytes. This is an engineering provenance
rule, not a claim that the project has used a formal clean-room team process.

## Verification runs

USER 2026-08-22: "don't do windowed runs please always run headless"

Every run started by an agent for verification, profiling, capture, or
diagnosis must use `./run.sh --headless` or a purpose-built headless tool. Do
not open a game window for a smoke test, including when checking the launcher;
exercise the launcher with its headless option instead.

## Python tooling

USER 2026-08-24: "all python should run through uv projects"

The repository root `pyproject.toml` and `uv.lock` are the only Python
dependency authority. Run project tools as `uv run --locked python <tool>`;
CMake generators and CTest use the same locked project. Do not select an
ambient interpreter, install dependencies outside the project, or add inline
script dependency metadata that creates a second environment.

## Host architecture

Use `${DUSKLIGHT_REPO}` when set, otherwise the sibling checkout at
`../../dusklight`, as the ownership reference. Adapt its cohesive host
subsystems rather than copying platform-specific implementations.

- Shared engine sources compose separately from locally generated title code.
  One executable links one exact title/revision module; do not link multiple
  generated images whose `_xstart`, `sub_*`, and `ppc_config.h` namespaces
  collide.
- The recompiler emits weak forwarding functions and retains `__imp__sub_*`
  bodies. Generated sources are sacrosanct: no alias-stripping or other
  post-generation mutation is allowed.
- A title adapter owns exact image identity, semantic override bindings,
  title-specific probes, pass hashes, save namespace, and scripted navigation.
  Unknown revisions refuse rather than falling back by title name or image size.
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
- `runtime/native_rhi.*` owns the title-neutral, PM4-independent semantic frame
  plan boundary. It must not absorb Vulkan execution, Xenos translation, or
  title-address bindings; host resource/pipeline execution belongs in a later
  focused native backend behind the retained compatibility arm.
- `runtime/native_rhi_backend.*` owns only the explicit host-backend execution
  contract and ordered frame transaction. A backend must provide real native
  resource/pipeline/submission ownership and may refuse a frame; no no-op
  backend is installed, and this interface does not authorize bypassing the
  retained compatibility renderer.

Pure state transitions belong behind production interfaces with focused tests.
Run `tools/check_source_structure.py`, its self-test, and the CTest suite before
landing host changes; new sources are capped mechanically and legacy monoliths
may not grow.
