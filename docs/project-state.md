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
| S008 | Bounded runtime interpreter fallback | partial | S006 | G001, G002 |
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
the pinned `x360port`/Xenia execution boundary for an asset-free synthetic discriminator,
owns a profile-authenticated normalized-image-to-flat-guest adapter, and composes that
adapter with one persistent runtime context, bounded variable-import storage, and the
title-owned `XGetAVPack` binding. The shared checked XEX2 inspector and real ignored-image
identity path are verified, but the product still refuses the gameplay target until the
remaining import bindings and runtime services are composed. The executor will prefer
dynarec and may use the bounded, counted fallback; current fallback coverage cannot prove
gameplay compatibility or performance.

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
title-neutral device-memory callback contracts. It validates title-reported
executable-write ranges and now automatically observes virtual writes to the
authenticated code range, exits an active translated call after a watched guest
store, invalidates touched cached functions before the next guest entry, and
preserves unrelated translations. The shared runtime also bounds translated
basic-block execution across nested guest calls and returns typed exhaustion and
invalidation results. A host import can refuse explicitly; the translated call
exits with its library, ordinal, and reason while incrementing a refusal count,
rather than continuing with an invented return value. The pinned Xenia fork now
routes compiled direct and indirect guest calls through invalidatable entries;
the shared synthetic regression modifies a leaf and observes its new result
through a previously translated caller. The updated shared contract also routes
a cached guest caller to a native override, preserves scoped original calls
through callee invalidation, propagates typed callback failure, and restores the
original call after override removal. The headless real Gears-image AddRef
discriminator now exercises the same native-override route from a nested guest
call and verifies restoration after removal. The pinned shared runtime now
refuses an invalid PPC opcode before host-code publication and routes
decoded-but-unimplemented fixtures through a bounded Xenia-owned interpreter. The fallback covers
`lswi`, integer immediates, big-endian scalar loads/stores, comparisons, and conditional branches,
and records entries, executed instructions, unsupported instructions, memory failures, and budget
exhaustion; the valid cached PPC function still executes through the JIT.

`runtime/gears1_guest_image.*` now consumes
the shared PE layout owner and seals a flat guest module after exact normalized-image
profile authentication. `runtime/gears1_runtime.*` composes that sealed module with a
single Xenia runtime context, persistent variable-import storage, the title-owned
`XGetAVPack` service, and fail-closed unknown function imports; its synthetic entry test
executes the authenticated adapter through Xenia's JIT and returns the configured AV pack.
The shared checked-XEX2 inspector also validates the real ignored container, normalized
image, import manifest, and helper-pattern evidence. Gap: wiring authenticated full-image
loading into the product, complete fallback ISA/control-flow coverage, title-specific
write/cache-control semantics, and complete product service composition remain missing.

### S007 — first Gears discriminator

Evidence: the synthetic Gears-addressed module executes through Xenia's JIT and
calls a typed `DbgPrint` binding; the pinned shared runtime separately proves
cached guest-to-native override/original-call routing against a synthetic guest leaf.
The runtime-composition test authenticates the synthetic profile image through the Gears
adapter, binds title-owned `XGetAVPack` ordinal 971, and returns its configured value through
Xenia's translated entry path.
A maintained headless discriminator authenticates the profile-matching ignored
image, maps its PE sections into guest virtual offsets, allocates and initializes
a caller-owned guest object and variable-import storage through `x360port`,
invokes a retained real-image function-import thunk through the same manifest,
and executes real leaf `0x82233668`: it returns `0x5`, the native override
calls the real original once, and reported executable-write invalidation restores
the original path. A second real-image fixture activates the leaf's own nested
guest call at `0x822336C0`: outer and inner calls both enter the native override,
then removal restores the nested original call and its inner refcount effect.
The production `Gears1GuestImage` adapter now authenticates
the same normalized image and import manifest; its synthetic CTest and real
ignored-input discriminator pass. The shared checked-XEX2 inspector provides the
container, normalized image, 236 logical imports under the correctly indexed
`xam.xex` and `xboxkrnl.exe` entries, and eight helper-pattern hits.
The first real-image function-import thunk reaches its title callback and now
returns `ImportServiceRefused` for an unsupported service; all
real-image variable imports resolve into bounded caller-owned guest storage.
The title-owned `XGetAVPack` binding for `xam.xex` ordinal 971 executes through
its real thunk and returns the configured AV-pack value. The real
`XamInputGetState` ordinal 401 thunk consumes the retained Gears input owner's
coherent remote-pad snapshot through shared `x360port` XAM serialization. The
headless ignored-XEX discriminator verifies all 16 guest bytes, a connected
null-pointer query, the one-local-user policy, disconnection clearing stale
state, and typed refusal of an unmapped state pointer. The asset-free remote
input test preserves packet and source-arbitration behavior. The real
`XamInputGetCapabilities` ordinal 400 thunk now reports the title's virtual-pad
capability policy as a checked 20-byte guest record; the headless discriminator
also proves disconnected zeroing, null-pointer bad arguments, and typed refusal
of unmapped memory. The superseded direct-memory handlers were removed. Gap:
vibration output and the remaining title-specific service/device semantics, plus
the complete product launch path, remain unimplemented.

### S008 — bounded interpreter fallback

Partial capability: after Xenia compilation refusal, `x360port` now selects the Xenia-owned
bounded interpreter and reports typed unsupported-instruction, memory, and instruction-budget
refusals. The synthetic runtime discriminator proves an invalid opcode is refused without host
code, and proves `lswi`, integer immediates, big-endian scalar loads/stores, comparisons, and
conditional branches execute with counted fallback entries/instructions. Explicit interpreter mode
remains diagnostic-only and fallback is not gameplay or performance evidence. Gap: the interpreter
currently covers only this narrow subset; complete PPC ISA semantics, safe guest control flow,
imports/devices, real-image fallback, and title gameplay remain open.

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
--expected-machine x86_64` gate uses the exact `x360port` and Xenia revisions
pinned by CMake and the workflow. It passed all eight CTests locally with Clang
on the pinned `x360port` revision `677f376fd18e9d59ff9d4a79a29f79d654e667d9`
with Xenia `b7b471a66120932ef3f738e004233061db6127a5`; the headless
real-image import/leaf discriminator passed separately against the ignored
user-supplied XEX, resolving 236 imports and executing the retained real leaf,
scoped original, nested guest-call override/removal, and executable invalidation.
Gears consumes the shared
driver-aware warning owner;
production CMake selection and real Clang/clang-cl probes accept modern C++ and
reject an unused parameter. That ownership change preserved every native compile
command, and the focused boundary and quality CTests passed afterward. Xenia's
standalone portability suite stays disabled in this consumer; its framework owns
that separate gate. This is synthetic native/JIT mechanism evidence, not Gears
gameplay conformance.
Gap: hosted qualification remains pending, including the known Windows tabulate
portability fix awaiting a fork-publication decision. Android remains absent
because no APK/runtime owner exists.
