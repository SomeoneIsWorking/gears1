# GearsUE3 engine-port guidance

The repository-wide rules in `../../AGENTS.md` apply here. Read
`../../shared/jit-common/docs/migration.md`, `docs/project-state.md`, and
`docs/codemap.md` before changing a subsystem. Update the nearest authority in
the same change when its answer changes.

## Product target

USER 2026-09-01: "there are two runtimes there, one native (WIP), one emulator, the game itself is still guest game so the wording 'recomp' just means the emulated runtime"

GearsUE3 is one native/dynarec engine port for the Xbox 360 Gears of War UE3
titles. The two cooperating execution paths are measured native overrides and
an emulated path that executes every other guest instruction through Xenia's
existing x64 or A64 Xenon dynarec. The gameplay product contains no interpreter
and has no interpreter or generated-code fallback.

The platform layer is `xenonport`, a narrow framework around Xenia `Memory`,
`Processor`, `ThreadState`, `RawModule`, typed imports, device-memory callbacks,
runtime overrides, and scoped original calls. Do not put Xenia behind
`jit-common` caches and do not write another PPC interpreter, decoder, or host
code emitter. Xenia owns Xenon translation and its code cache. `xenonport` owns
the embedding contract and must account explicitly for Xenia's process-global
memory, MMIO, and clock assumptions.

Gears 1 is the first conformance target. Its first implementation discriminator
is deliberately smaller than boot: execute the real leaf at `0x8222E868`, bind
and call `DbgPrint` through a typed import, and prove disabled, enabled, and
`super` override paths through Xenia. Disabled and `super` both execute the
original guest body through the dynarec; `super` suppresses only the current
override for one call. This discriminator does not authorize deletion of the
old path. Preserve it without regenerating, building, or running it until the
Xenia path reaches representative interactive gameplay and passes the migration
gate in `../../shared/jit-common/docs/migration.md`; then delete XenonRecomp,
generated PPC modules, function maps, generator-only configuration/tests, and
their documentation together.

Guest addresses, image identity, shader hashes, menu walks, and diagnostics
belong to a title/revision adapter. Shared engine or platform code must not
acquire Gears 1 policy merely because it is the first target. Finish Gears 1's
declared compatibility and performance gates before beginning title-specific
work for another Gears game.

## Clean distribution boundary

USER 2026-08-22: "I don't want to provide copyrighted material in anyway either from a private repo or a download link, I only want to provide absolute clean code and others should only provide the ROMs"

The public repository contains independently authored source, compatible
open-source dependencies with their required notices, and factual
interoperability metadata only. It must not contain or fetch UE3 source, game
code/assets, extracted files, decoded shader listings, decompiler output,
generated guest bodies, or caches derived from a title. A user-owned disc/image
is the only copyrighted provisioning input.

Disc-derived output belongs under ignored `scratch/titles/<fingerprint>/` and
must be regenerable. Private UE3 source may settle a conceptual question for a
developer, but no expression, comment, declaration, patch context, mechanical
translation, or generated artifact from it may enter this repository.

## Verification runs

USER 2026-08-22: "don't do windowed runs please always run headless"

Every run started by an agent for verification, profiling, capture, or diagnosis
must be headless. Do not open a game window for a smoke test. During the
migration, do not invoke any path that builds, generates, or launches the
XenonRecomp product. New comparison evidence comes from the independent Xenia
oracle, hardware, binary analysis, or a separately built diagnostic target.

## Python tooling

USER 2026-08-24: "all python should run through uv projects"

The repository root `pyproject.toml` and `uv.lock` are the only Python
dependency authority. Run project tools as `uv run --locked python <tool>`;
CMake generators and CTest use the same locked project. Do not select an
ambient interpreter or create a second environment.

## Host architecture

This repository's `docs/codemap.md` is the ownership authority. Keep the host
self-contained: the entry point composes focused modules, each subsystem owns
one coherent responsibility and its lifetime, and shared/title/platform policy
crosses narrow explicit interfaces. Split a mixed or oversized owner before
extending it; do not create forwarding fragments, catch-all helpers, or copy a
platform implementation from another game project. Reusable cross-title Xbox
360 execution belongs in `xenonport`; Gears-specific behavior stays here.

- `run.sh` remains a slim locked-environment shim. Its next shipping route must
  provision the authenticated runtime image and launch the xenonport/Xenia
  product without offline translation. Until that route exists, documentation
  must not present the static launcher as the product.
- A title adapter owns exact image identity, semantic override bindings,
  title-specific probes, pass hashes, save namespace, and scripted navigation.
  Unknown revisions refuse rather than falling back.
- Normal calls honor the runtime override table. `super` suppresses only the
  current override and re-enters the original guest address through Xenia.
  Override mutation invalidates translated call paths that captured an older
  decision.
- `runtime/vd_null_gpu.cpp` composes guest GPU dispatch with host subsystems; it
  must not absorb their implementations.
- `runtime/input.cpp` owns controller sources, arbitration, and the guest-facing
  controller snapshot.
- Lucent owns reusable loopback HTTP transport. Gears code owns only routes and
  translations through narrow input/probe interfaces.
- `runtime/graphics_probe.cpp` owns on-demand readback state and publication;
  shipping render passes do not depend on HTTP.
- `runtime/gpu_scanout.cpp` owns finished-frame staging and scan-out transforms;
  presentation orchestration does not own swapchain resources.
- `runtime/native_rhi_resources.*` owns title-neutral guest-object identity and
  non-boundary lifetime bookkeeping; API allocation and retirement belong to
  the backend.
- `runtime/native_rhi.*` owns the PM4-independent semantic frame plan;
  title-address bindings, Xenos translation, and host backend execution stay in
  their focused owners.

Pure state transitions belong behind production interfaces with focused tests.
Run the structure check, its self-test, and the relevant focused tests before a
host change; run the combined gate once when semantic edits are frozen.
