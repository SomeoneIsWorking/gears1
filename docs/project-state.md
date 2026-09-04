# Project state

Comparison baseline: retail Xbox 360 Gears titles running in an emulator.

This inventory reports observable capabilities independently of the product goals.
`verified` means exercised by the cited test or recorded real-title evidence;
`partial` names the remaining gap; `missing` means no product implementation exists.

| ID | Capability | State | Evidence or exact gap |
|---|---|---|---|
| S001 | Exact Gears 1 executable identity and normalized image validation | verified | `config/titles/gears1.toml`, `tools/title_identity.py`, and the title-identity tests fail closed on container and normalized-image hashes. |
| S002 | Bounded user-image/archive provisioning without derived guest source | partial | GDF extraction, archive bounds, and identity are tested; `./run.sh` truthfully refuses because `x360port` has no executor target yet. |
| S003 | Executor-independent native renderer and RHI contracts | partial | Focused native frame, draw, resolve, resource, and shader tests build without a CPU executor; no live Xenia-fed native frame exists. |
| S004 | Native Gears 1 audio-mix operation | partial | `runtime/titles/gears1/audio_mix.*` owns the independently authored kernel and known address `0x825F2D40`; dispatch and differential qualification through `x360port` are missing. |
| S005 | Native notified operation-kind-3 GPU ticket wait | partial | `runtime/gpu_ticket_wait.*` and `runtime/wait_probe.*` retain the host synchronization contract; guest-address dispatch through `x360port` is missing. |
| S006 | Xenia-backed `x360port` gameplay executor | missing | The pinned shared repository validates authenticated images/import tables but exposes no executor target. |
| S007 | Gears 1 leaf/import/override discriminator | missing | Must execute original guest leaf `0x8222E868`, a typed `DbgPrint` import, a native override, a scoped original call, and cache invalidation through Xenia. |
| S008 | Bounded runtime interpreter fallback | missing | Dynarec must remain default. Fallback may run only when compilation fails, an instruction is unsupported, or generated host code is unsafe; every transition and executed block must be counted. Explicit interpreter mode is diagnostic-only. |
| S009 | Representative interactive Gears 1 gameplay | missing | No current product executable exists. Prior title evidence is migration input, not a current gameplay result. |
| S010 | Apple Silicon macOS A64 execution | missing | Needs A64 code emission plus executable-memory, instruction-cache, ABI, exception, and packaging qualification. |
| S011 | Android arm64-v8a A64 execution | missing | Needs A64 code emission plus executable-memory, instruction-cache, ABI, lifecycle, and APK packaging qualification. |
| S012 | Complete native RHI frontend | missing | Existing native pieces do not yet bypass guest command construction or prove frame parity. |
| S013 | Native 8.33 ms / 120 fps renderer budget | missing | Requires representative gameplay on named hardware after S012. |
| S014 | Gears 2, Gears 3, and Judgment exact-revision conformance | missing | Gears 1 remains the active title until its compatibility and performance gates complete. |
| S015 | Generated guest-source product and translator-only surfaces absent | verified | Build graph, launcher, tests, tools, and docs have no generated guest corpus, function map, offline CPU translator, or selectable old product. The migration-boundary gate enforces this across prospective first-party paths. |
| S016 | Independently authored shared UE3/Xbox contract | missing | `shared/x360ue3` does not exist. Its first contract must be proven from an executing Xenia context; it must not copy or depend on `shared/ue3`. |

## Current focus

S006 is the current focus. Gears 1 is the only active title. The repository intentionally
builds executor-independent native components but refuses the named gameplay target until
the pinned `x360port` revision exposes the typed Xenia executor boundary. The executor will
prefer dynarec and may use the bounded, counted fallback in S008; fallback coverage cannot
prove gameplay compatibility or performance.
