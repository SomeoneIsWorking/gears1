# Project goals

These goals define why GearsUE3 exists and the durable outcomes it must reach. They do not report
what currently works; `docs/re-frontier.md` records the evidence-dependent implementation frontier,
`docs/codemap.md` records ownership, and `docs/issues/` records atomic findings and work points.

## G001 — Ship one shared native GearsUE3 engine

### Why

Gears of War's Xbox 360 executable is evidence for engine behavior, not the architecture the PC
product should execute forever. The product is a native engine for the Gears UE3 family. Static
recompilation provides the exact guest ABI, compatibility path, and migration oracle while native
engine subsystems progressively own the shipping execution path.

### Success conditions

- Shared native owners implement rendering/RHI, audio, input, storage, configuration, timing, and
  presentation above exact title/revision adapters.
- Gears of War, Gears of War 2, Gears of War 3, and Gears of War: Judgment can each provision and
  run as a separately linked exact-revision product without copying title policy into the engine.
- The normal product path executes the grounded native owners; retained recompiled bodies remain
  callable in the same binary as compatibility, comparison, and regression controls.
- Adding a supported title or revision primarily adds a generated title module, factual bindings,
  and conformance evidence rather than a second engine implementation.

### Constraints

- One executable links one exact generated title module because generated guest symbol namespaces
  and image constants collide.
- Generated recomp output is never edited, and every native replacement has a runtime original/native
  seam with an explicit super-call until its parity gate authorizes native execution.
- Shared engine code contains no Gears 1 guest addresses, hashes, or revision policy.
- The shipping native engine must not emulate a console GPU: no GameCube GPU emulation and no
  Xbox 360/Xenos/PM4 GPU emulation may be part of its final rendering path. Console GPU behavior
  may remain in the retained compatibility oracle used for migration and conformance only.

### Non-goals

- A Gears 1-only source port.
- Treating the PM4/Xenos compatibility renderer or the recompiled title executable as the final
  engine architecture.
- Making console-GPU emulation a required product dependency, default renderer, or performance
  fallback for the native engine.
- Reimplementing unrelated general-purpose Unreal Engine editor or content-authoring tools.

## G002 — Preserve exact behavior while migrating ownership natively

### Why

Native speed is useful only when the engine preserves the guest-visible behavior on which each title
depends. The retained recomp path makes behavior measurable instead of requiring ungrounded rewrites.

### Success conditions

- Each native subsystem has complete ordered semantic observations at its boundary and a comparer
  that has rejected a deliberately wrong control.
- Faithful original/native runs agree on guest-visible state, resource lifetime, frame identity,
  audio/input/storage effects, and pixels for the workloads that authorize each replacement.
- Unknown executable revisions, ambiguous bindings, missing evidence, and unsupported operations
  refuse rather than selecting a nearby profile or silently changing behavior.
- Every compatibility claim names the exact title revision and the independently passed gate.

### Constraints

- Private UE3 source may not contribute code, declarations, comments, patch context, or mechanically
  translated expression to the public implementation.
- Gears 1 evidence does not automatically establish another title's policy or compatibility.

### Non-goals

- Pixel similarity scores without exact-state or deliberately differing instrument controls.
- Declaring a native path complete because it boots, draws a frame, or passes a single scene.

## G003 — Reach native-PC frame cost and responsiveness

### Why

The compatibility path reconstructs Xenos command processing, EDRAM behavior, and console pass
structure that a native PC engine should not pay for. A 2006-era title should not inherit emulator
overhead as its permanent performance ceiling.

### Success conditions

- The native renderer sustains a documented 8.33 ms / 120 frames-per-second budget on the
  designated target hardware and representative gameplay workloads, including submission and
  retirement.
- Presentation is uncapped by the compatibility architecture and remains bounded, monotonic, and
  free of stale-frame glitches under load.
- Per-title simulation/frame-production limiters are identified semantically and exposed through a
  faithful/enhanced runtime seam; measured simulation cadence is reported separately from renderer
  capacity.
- Performance gates use same-run controls, warm representative workloads, and output/state parity so
  an optimization cannot pass by dropping work.

### Constraints

- The 8.33 ms target applies to the native engine path, not as a requirement to micro-optimize the
  PM4 oracle into the product architecture.
- Timing enhancements do not speed the general guest clock or patch unexplained constants.

### Non-goals

- Counting repeated presentations as newly simulated frames.
- Hiding a 30 Hz title producer behind a 60 Hz vblank or frame interpolation.

## G004 — Distribute clean code with a user-owned game image as the only restricted input

### Why

GearsUE3 must be independently distributable without shipping, fetching, or directing users to
copyrighted game material or private engine source.

### Success conditions

- A fresh clone plus documented native dependencies, `uv`, and a user-owned disc/image can provision
  and launch the default product through `./run.sh`.
- All disc-derived source, assets, caches, and executable data remain under ignored content-addressed
  storage and can be regenerated locally.
- The tracked tree and full public history pass the distribution-clean gate.

### Constraints

- No ROM, disc image, extracted asset, generated recomp body, decompiler output, private-source
  dependency, or game download link enters the repository.
- Provisioning selects an exact supported revision by cryptographic identity and fails closed.

### Non-goals

- Shipping pre-generated title modules for convenience.
- Requiring Ghidra or private development tools on the player setup path.
