# Project state

Comparison baseline: retail Xbox 360 Gears titles running in an emulator.

This inventory reports observable capabilities independently of the product goals.
`verified` means exercised by the cited test or recorded real-title evidence;
`partial` names the remaining gap; `missing` means no product implementation exists.

| ID | Capability | State | Dependencies | Goals |
|---|---|---|---|---|
| S001 | Exact Gears 1 executable identity and normalized image validation | verified | — | G002, G004 |
| S002 | Bounded user-image/archive provisioning without derived guest source | partial | S001 | G001, G004 |
| S003 | Executor-independent native renderer and RHI contracts | partial | — | G001, G002, G003 |
| S004 | Native Gears 1 audio-mix operation | partial | S006 | G001, G002 |
| S005 | Native notified operation-kind-3 GPU ticket wait | partial | S006 | G001, G002 |
| S006 | Xenia-backed `x360port` execution boundary | partial | S001 | G001, G002 |
| S007 | Gears 1 leaf/import/override discriminator | partial | S001, S006 | G001, G002 |
| S008 | Bounded runtime interpreter fallback | missing | S006 | G001, G002 |
| S009 | Representative interactive Gears 1 gameplay | missing | S002, S003, S004, S005, S006, S007, S008 | G001, G002 |
| S010 | Apple Silicon macOS A64 execution | missing | S006 | G001, G004 |
| S011 | Android arm64-v8a A64 execution | missing | S006 | G001, G004 |
| S012 | Complete native RHI frontend | missing | S003, S006 | G001, G002, G003 |
| S013 | Native 8.33 ms / 120 fps renderer budget | missing | S009, S012 | G003 |
| S014 | Gears 2, Gears 3, and Judgment exact-revision conformance | missing | S009, S013 | G001, G002 |
| S015 | Generated guest-source product and translator-only surfaces absent | verified | — | G001, G002, G004 |
| S016 | Independently authored shared UE3/Xbox contract | verified | S006 | G001 |
| S017 | Asset-free native/JIT boundary CI | partial | S006 | G001, G002, G004 |

## Current focus

S006 is the current focus. Gears 1 is the only active title. The repository now consumes
the pinned `x360port`/Xenia execution boundary for an asset-free synthetic discriminator
and now owns a profile-authenticated normalized-image-to-flat-guest adapter. The shared
checked XEX2 inspector and real ignored-image identity path are verified, but the product
still refuses the gameplay target until the remaining import bindings and runtime services
are composed. The executor will prefer dynarec and may use the bounded, counted
fallback; fallback coverage cannot prove gameplay compatibility or performance.

## Capability details

### S001 — exact title identity

Evidence: `config/titles/gears1.toml`, `tools/title_identity.py`, and the
title-identity tests fail closed on container and normalized-image hashes.

### S002 — bounded provisioning

Evidence: GDF extraction, archive bounds, and identity are tested. The shared checked-XEX2
inspector now validates the real ignored Gears 1 container and produces the exact
profile-authenticated normalized image consumed by the adapter. Gap: the inspector and
runtime-service composition are not wired into `./run.sh`; complete install validation and
product provisioning still refuse to launch until the full runtime path exists.

### S003 — executor-independent native rendering

Evidence: focused native frame, draw, resolve, resource, and shader tests build
without a CPU executor. Gap: no live Xenia-fed native frame exists.

### S004 — native audio mix

Evidence: `runtime/titles/gears1/audio_mix.*` owns the independently authored
kernel and known address `0x825F2D40`. Gap: dispatch and differential
qualification through `x360port` are missing.

### S005 — native GPU ticket wait

Evidence: `runtime/gpu_ticket_wait.*` and `runtime/wait_probe.*` retain the host
synchronization contract. Gap: guest-address dispatch through `x360port` is
missing.

### S006 — Xenia-backed execution

Evidence: `test_gears1_dynarec_boundary` consumes exact shared-runtime and Xenia
revisions, maps an aligned authenticated synthetic image whose code and entry
point use the retained Gears leaf address, and crosses JIT to typed import to
native code. The pinned shared runtime also now exposes and tests image-scoped
native override, scoped original-call, exact-entry invalidation, and
title-neutral device-memory callback contracts. It also validates a
title-reported executable-write range, invalidates touched cached functions,
and preserves unrelated translations. `runtime/gears1_guest_image.*` now consumes
the shared PE layout owner and seals a flat guest module after exact normalized-image
profile authentication. The shared checked-XEX2 inspector also validates the real ignored
container, normalized image, import manifest, and helper-pattern evidence. Gap: wiring
authenticated full-image loading into the product, automatic write observation, internal
guest-call invalidation, and product service composition remain missing.

### S007 — first Gears discriminator

Evidence: the synthetic Gears-addressed module executes through Xenia's JIT and
calls a typed `DbgPrint` binding; the pinned shared runtime separately proves
the native override/original-call dispatch seam against a synthetic guest leaf.
A headless probe mapped the profile-matching ignored image's PE sections into
guest virtual offsets, seeded a synthetic object field, and executed the real
`0x8222E868` body: it returned `0x5`, the native override called the real
original once, and reported executable-write invalidation restored the original
path. The production `Gears1GuestImage` adapter now authenticates the same
normalized image and performs the PE-to-flat mapping; its synthetic CTest and
real ignored-input adapter run pass. The shared checked-XEX2 inspector now provides
the container, normalized image, 236 logical imports, and eight helper-pattern hits.
Gap: real import bindings and device services, a caller-owned object/runtime path,
and the complete product launch path remain.

### S008 — bounded interpreter fallback

Missing capability: permit fallback only after compilation failure, an
unsupported guest instruction, or unsafe generated host code, with every
transition and executed block reason-labelled and counted. Explicit interpreter
mode remains diagnostic-only.

### S009 — representative gameplay

Missing capability: reach and independently compare representative interactive
Gears 1 gameplay through the current product. Prior title evidence is migration
input, not a current gameplay result.

### S010 — Apple Silicon A64

Missing capability: qualify A64 code emission, executable memory,
instruction-cache coherence, host ABI, exceptions, and packaging on Apple
Silicon macOS.

### S011 — Android A64

Missing capability: qualify A64 code emission, executable memory,
instruction-cache coherence, host ABI, lifecycle, and APK packaging on Android
`arm64-v8a`.

### S012 — complete native RHI

Missing capability: bypass guest command construction through the native RHI
and prove frame parity. Existing native pieces do not establish that result.

### S013 — native renderer budget

Missing capability: sustain and document an 8.33 ms / 120 fps native-renderer
budget during representative gameplay on named hardware.

### S014 — later Gears titles

Missing capability: qualify exact revisions of Gears 2, Gears 3, and Judgment
after Gears 1 compatibility and performance gates complete.

### S015 — generated guest-source product absent

Evidence: the build graph, launcher, tests, tools, and docs contain no generated
guest corpus, function map, offline CPU translator, or selectable old product.
The migration-boundary gate checks prospective first-party paths.

### S016 — shared UE3/Xbox contract

Evidence: the pinned public `shared/x360ue3` revision provides title-supplied
ABI/binding-schema validation plus object/resource/frame lifetime and semantic
RHI ordering contracts. Gears consumes it in `test_gears1_ue3_contract` with
positive and controlled-negative cases. Gap: the contract is not yet composed
with the authenticated real-image runtime and native RHI backend.

### S017 — native/JIT boundary CI

Evidence: the immutable workflow defines the Gears-owned production-boundary
discriminator for Linux x86-64, Windows x86-64, and macOS arm64. It also executes
the exact/clean dependency and bootstrap contracts, and the canonical C++ quality
owner formats maintained source and lints the built first-party discriminator.
The canonical `tools/verify_dynarec_boundary.py --x360port-root ../../shared/x360port
--expected-machine x86_64` gate passed all six CTests locally with Clang 22.1.8
against `x360port` revision `5c3c131a619381f0e99bbeb39123711c6d983e17` and Xenia
revision `5d14ad55e9e4a004382585b58996ffedf2f6e35e`, the exact inputs required by
CMake and the workflow. The unchanged second build performed zero compilations
(`ninja: no work to do`). Gears consumes the shared driver-aware warning owner;
production CMake selection and real Clang/clang-cl probes accept modern C++ and
reject an unused parameter. That ownership change preserved every native compile
command, and the focused boundary and quality CTests passed afterward. Xenia's
standalone portability suite stays disabled in this consumer; its framework owns
that separate gate. This is synthetic native/JIT mechanism evidence, not Gears
gameplay conformance.
Gap: hosted qualification remains pending, including the known Windows tabulate
portability fix awaiting a fork-publication decision. Android remains absent
because no APK/runtime owner exists.
