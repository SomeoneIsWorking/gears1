---
id: 170
title: Xenia dynarec product boundary is not wired
status: open
symptom: GearsUE3 exercises x360port/Xenia synthetically, but the authenticated full-image product and runtime services are not composed
tags: dynarec,xenia,x360port,x360ue3,migration
state_items: S002,S006,S007,S008,S009,S010,S011
created: 2026-09-04
updated: 2026-09-12
---

## Root cause

The previous architecture made a generated C++ corpus the guest execution
owner. The new product contract requires runtime translation from the
authenticated user executable. The shared `x360port` embedding boundary,
checked-XEX inspector, Gears-owned flat-image adapter, and synthetic
discriminator now exist, but the product still lacks composition of real import
and device services with guest-call routing, mid-call invalidation, exits, and
fallback.

## Required work

Extend the current `x360port` ownership of Xenia `Memory`, `Processor`,
`ThreadState`, `RawModule`, typed imports, and guest calls with device-memory
callbacks, runtime overrides, scoped original calls, bounded exits, and
Xenia-owned invalidation. Reuse Xenia's x64 and A64 dynarecs; do not write a
second PPC interpreter or put Xenia behind `jit-common` code-memory/cache owners.
Expose Xenia's existing interpreter only as the bounded, reason-labelled
fallback and explicit diagnostic mode defined by the project goals.

The asset-free Gears discriminator now translates an aligned synthetic image
whose code and entry point use retained leaf address `0x8222E868`, calls a typed
`DbgPrint` import, and returns to native code. The maintained discriminator now
executes the authenticated real leaf and disabled/enabled/scoped-original
override paths through Xenia. Then expand to
representative interactive gameplay and the full migration gate.

The executor must select Xenia's A64 dynarec for both Apple Silicon macOS and
Android `arm64-v8a`, with dynarec selected by default and only the bounded,
reason-labelled fallback policy described in the project goals. Qualify executable
memory, instruction-cache coherence, host ABI, and runtime packaging separately
on those two platforms rather than inferring one from the other.

## Break-first condition

Satisfied: the retired translator submodule, generated-module build path, precomputed
function maps/import profile, generation configuration/tools/tests, obsolete entry
and indirect dispatch, and stale-product selector are gone. Independent evidence,
native subsystem contracts, and the reusable checked-XEX parser contract remain.
The launcher and named product target now refuse at missing full-image and
runtime-service composition over `x360port`; there is no compatibility CPU
selector.

## Progress note — 2026-09-08

The pinned `x360port` revision now exposes a title-owned, image-scoped native
override entry point. Its handler can call the matching original guest address
through Xenia without recursive override dispatch, and installing or removing
the override invalidates that exact Xenia entry. The shared runtime test proves
the wrapped return value, original-call and invalidation counters, and restored
dynarec execution. The same revision now owns validated title-neutral device
ranges through Xenia MMIO and proves PPC load/store callbacks with telemetry.
It also validates title-reported executable-write ranges and invalidates the
affected cached Xenia functions while preserving unrelated entries.
The shared runtime now also traps writes to the authenticated virtual code range,
exits an active translated call after the watched guest store, coalesces the
write before the next guest-call boundary, resets affected Xenia module
functions, and re-translates modified bytes; the synthetic self-modifying test
proves the typed invalidation result and changed target result. This closes only
reusable entry, device, automatic write-observation, and mid-call invalidation
seams; it does not prove device services, fallback, or gameplay.

## Image adapter — 2026-09-08

`x360port::MapPeImage` now owns the title-neutral conversion from the checked
XEX inspector's normalized PE container to the flat guest image that
`RuntimeContext` maps. `runtime/gears1_guest_image.*` authenticates the exact
profile image digest and geometry, then seals the flat image, executable range,
and import manifest as one `GuestModule`. Its synthetic CTest and a local run
against the ignored profile-matching image passed. The shared inspector now
produces the exact normalized-image digest and geometry, 236 logical imports,
and one hit for each of the eight helper patterns. Real import/service bindings
remain open; the caller-owned object path is now proven, and the title-owned
`XGetAVPack` binding is exercised through its real thunk, but this is still an
adapter milestone, not title conformance.

## Checked XEX inspector — 2026-09-08

The shared `x360port` checked-XEX2 inspector and title-owned guest allocation
contract are pinned at `782a7d446979109574645ebec3eaacd57944d0ba`, with Xenia at
`d77aad9116c936cf1c7a40231f210d17094bed9e`. Against the ignored profile-matching
Gears 1 XEX it produces the exact normalized-image digest and geometry, 236 logical
imports, and one hit for each of the eight helper patterns. Malformed-container
preflight cases and compressed-loader bounds are covered by the shared tests. This
is checked loader and title-identity evidence, not product conformance: real import
bindings, device/runtime services, and `./run.sh` provisioning remain open. The
maintained real-image probe now also resolves every variable import into
caller-owned guest storage, invokes the first retained function-import thunk,
and observes its title callback; that proves routing only, not service
semantics.

## Real-leaf discriminator — 2026-09-08

A maintained headless discriminator loads the ignored image whose SHA-256 matches
the Gears 1 profile, maps its PE sections into guest virtual offsets, allocates
and initializes a caller-owned guest object and variable-import storage through
`x360port`, invokes a retained real-image function-import thunk through the
authenticated manifest, and enters `0x82233668` through the pinned Xenia
dynarec. The real body returned `0x5`; its native override called the real
original once, and reported executable-write invalidation restored the original
path. This grounds the real leaf body and shared dispatch contracts; the shared
synthetic runtime also proves automatic virtual executable-write observation and
retranslation. It is not title conformance: the product still
needs to wire the checked full-image path to real imports/services and the
complete product launch path.

## Shared UE3 contract — 2026-09-08

The public `shared/x360ue3` repository now owns the title-neutral ABI and
binding-schema checks, semantic RHI operation ordering, and object/resource/frame
lifetime transitions. Gears pins and consumes that revision in an asset-free
contract test. It does not contain Gears policy or replace the missing product
authenticated XEX/import/service composition.
