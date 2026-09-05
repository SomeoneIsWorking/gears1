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
| S016 | Independently authored shared UE3/Xbox contract | missing | S006 | G001 |
| S017 | Asset-free native/JIT boundary CI | partial | S006 | G001, G002, G004 |

## Current focus

S006 is the current focus. Gears 1 is the only active title. The repository now consumes
the pinned `x360port`/Xenia execution boundary for an asset-free synthetic discriminator,
but refuses the gameplay target until the authenticated full-image adapter and runtime
services are composed. The executor will prefer dynarec and may use the bounded, counted
fallback; fallback coverage cannot prove gameplay compatibility or performance.

## Capability details

### S001 — exact title identity

Evidence: `config/titles/gears1.toml`, `tools/title_identity.py`, and the
title-identity tests fail closed on container and normalized-image hashes.

### S002 — bounded provisioning

Evidence: GDF extraction, archive bounds, and identity are tested. Gap:
`./run.sh` refuses because the authenticated full-image adapter and runtime-service
composition are not wired.

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
native code. Gap: full-image loading, device callbacks, and product service
composition remain missing.

### S007 — first Gears discriminator

Evidence: the synthetic Gears-addressed module executes through Xenia's JIT and
calls a typed `DbgPrint` binding. Gap: the real authenticated leaf body, native
override, scoped original call, and cache invalidation remain missing.

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

Missing capability: create the first independently authored `shared/x360ue3`
contract from an executing Xenia context. It must not copy or depend on
`shared/ue3`.

### S017 — native/JIT boundary CI

Evidence: the immutable workflow defines the Gears-owned production-boundary
discriminator for Linux x86-64, Windows x86-64, and macOS arm64. It also executes
the exact/clean dependency and bootstrap contracts, and the canonical C++ quality
owner formats maintained source and lints the built first-party discriminator.
The canonical `tools/verify_dynarec_boundary.py --x360port-root ../../shared/x360port
--expected-machine x86_64` gate passed all four CTests locally with Clang 22.1.8
against `x360port` revision `4dc929a47cf462ed654e8f0384cb8edca279881f` and Xenia
revision `f04847eb6875e72f889a9e30aac48b7fe51f5fe2`, the exact inputs required by
CMake and the workflow. The unchanged second build performed zero compilations
(`ninja: no work to do`). This is synthetic native/JIT mechanism evidence, not
Gears gameplay conformance.
Gap: hosted results remain unverified until the commit is pushed; Android remains
absent because no APK/runtime owner exists.
