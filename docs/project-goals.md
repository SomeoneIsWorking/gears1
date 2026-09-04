# Project goals

These goals define durable product outcomes. `docs/project-state.md` records
current coverage, `docs/re-frontier.md` records the evidence chain, the codemap
records ownership, and issues record atomic work.

## G001 — Ship one shared native/dynarec GearsUE3 engine

### Why

The product should preserve the original game while moving deliberately owned
subsystems to native code. Recreating gameplay logic from scratch or compiling a
generated per-title source corpus would make the original executable cease to
be the runtime authority.

### Success conditions

- Gears of War, Gears 2, Gears 3, and Judgment each run from an authenticated
  user-owned image through one shared engine and exact title/revision adapters.
- Every non-native guest path executes on demand through Xenia's existing x64
  or A64 Xenon dynarec.
- Apple Silicon macOS and Android `arm64-v8a` select Xenia's A64 dynarec by
  default and pass separate executable-memory, instruction-cache coherence,
  host-ABI, exception, and runtime-packaging qualification.
- A bounded interpreter fallback may run only after a compilation failure,
  unsupported guest instruction, or unsafe generated host block. Every fallback
  transition and executed block is reason-labelled and counted; explicit
  interpreter mode is diagnostic-only, and fallback coverage cannot satisfy a
  gameplay or performance gate.
- The gameplay product contains no offline CPU translator, generated guest
  source, prebuilt title substrate, or generated-function fallback.
- Native overrides and scoped original calls compose through one image-aware
  runtime dispatcher; an original call re-enters Xenia at the guest address.

### Constraints

- `x360port` wraps Xenia `Memory`, `Processor`, `ThreadState`, `RawModule`,
  typed imports, device callbacks, runtime overrides, and original calls. It
  does not replace Xenia's decoder, host backends, or code cache with
  `jit-common` machinery.
- `x360ue3` depends only on `x360port` interfaces and owns reusable UE3 Xbox
  ABI/RHI/lifetime contracts. `GearsUE3` owns all Gears-family behavior and
  exact title/revision bindings. The local `shared/ue3` reference checkout is
  not a build, runtime, packaging, or distribution dependency.
- Shared engine/platform code contains no Gears 1 address, hash, or revision
  policy.
- Gears 1 is completed before title-specific implementation begins for another
  Gears title.

### Non-goals

- A Gears 1-only port, a gameplay rewrite, a generated-code product, or a
  second Xenon CPU implementation.
- Treating boot, a leaf-function test, or nonzero translated blocks as gameplay
  compatibility.

## G002 — Preserve exact behavior across the execution-owner migration

### Why

Replacing the generated-source CPU product with Xenia must preserve title behavior
and the already grounded native seams. A successful build or early boot cannot
certify this boundary.

### Success conditions

- The first Gears 1 discriminator executes real leaf `0x8222E868`, a typed
  `DbgPrint` import, and disabled/enabled/`super` override paths through Xenia.
- The authenticated dynarec product reaches representative interactive Gears 1
  gameplay at least as far as the current verified frontier with native owners
  active.
- CPU state, relevant memory, imports, timing/interrupts, devices, audio,
  rendering, and frame identity are compared against an independent oracle at
  the boundary under test, with positive and controlled-negative invalidation
  cases where executable mappings can change.
- Before dynarec implementation resumes, the retired translator, generated guest modules,
  function maps, generator-only seeds/tests/configuration, and their methodology
  are deleted. The build may fail only at the explicit missing
  `x360port` executor boundary until the replacement lands.

### Constraints

- Independently useful binary/behavior evidence and native subsystem contracts
  survive the break-first deletion; the old product does not survive as a
  runnable comparison arm.
- Unknown revisions, missing typed imports, stale override/cache state, and
  fallback reasons outside the bounded policy refuse with exact context.
- Private UE3 source contributes no public code or mechanically translated
  expression.

### Non-goals

- Declaring migration complete from the first discriminator, logos, menus,
  attract loops, or FMV.
- Using the old generated product as new comparison evidence.

## G003 — Reach native-PC frame cost and responsiveness

### Why

The completed engine should not retain Xbox 360 CPU or GPU reconstruction work
for subsystems whose behavior has been faithfully moved to native owners.

### Success conditions

- The native renderer sustains a documented 8.33 ms / 120 fps budget on named
  target hardware and representative gameplay, including submission and
  retirement.
- Presentation is bounded and monotonic, and per-title simulation cadence is
  measured separately from renderer capacity.
- Optimizations retain same-run state/output controls and cannot pass by
  dropping work.

### Constraints

- Timing enhancements use a semantic faithful/enhanced seam; they do not speed
  the general guest clock or patch unexplained constants.
- Compatibility measurements guide migration but do not redefine the native
  product's performance target.

### Non-goals

- Counting repeated presentations as simulated frames or hiding a 30 Hz
  producer behind a faster vblank.

## G004 — Distribute clean code with a user-owned game image as the only restricted input

### Why

The project must be independently distributable without shipping or fetching
copyrighted game material, private engine source, or derived guest code.

### Success conditions

- A fresh clone plus documented native dependencies, `uv`, and a user-owned
  disc/image provisions and launches the x360port/Xenia product through
  `./run.sh` with no offline translation step.
- Disc-derived executable bytes, assets, and runtime caches stay ignored and
  disposable; compiler output stays under `build/`.
- The tracked tree and full public history pass the clean-distribution gate.

### Constraints

- No ROM/disc image, extracted asset, derived guest source, decompiler output,
  private-source dependency, or game download link enters the repository.
- Provisioning validates exact container and normalized-image identity before
  title policy activates.

### Non-goals

- Shipping a generated module or persistent JIT cache as a fresh-install
  prerequisite.
- Requiring Ghidra or another maintainer-only analysis tool on the player path.
