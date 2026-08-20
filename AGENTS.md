# Gears 1 port guidance

The repository-wide rules in `../../AGENTS.md` apply here. Consult
`docs/codemap.md` before changing a subsystem and update it in the same commit.

## Host architecture

Use `${DUSKLIGHT_REPO}` when set, otherwise the sibling checkout at
`../../dusklight`, as the ownership reference. Adapt its cohesive host
subsystems rather than copying platform-specific implementations.

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

Pure state transitions belong behind production interfaces with focused tests.
Run `tools/check_source_structure.py`, its self-test, and the CTest suite before
landing host changes; new sources are capped mechanically and legacy monoliths
may not grow.
