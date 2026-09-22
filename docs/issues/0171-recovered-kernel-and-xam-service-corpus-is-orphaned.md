---
id: 171
title: Recovered kernel and XAM service corpus is orphaned from the build
status: open
symptom: 171 recovered `__imp__` service handlers across 21 files compile into no target and reference a header the migration deleted; the authenticated image imports 236 services, 168 of its 226 function imports have a recovered handler, and three are bound
tags: dynarec,x360port,imports,migration
state_items: S006,S007,S009
created: 2026-09-22
updated: 2026-09-22
---

## Finding

`runtime/kernel_*.cpp`, `runtime/xam_*.cpp`, `runtime/xnet_null.cpp`,
`runtime/xaudio_null.cpp`, `runtime/vd_null_gpu.cpp`, and `runtime/hle_d3d.cpp`
hold 171 distinct `__imp__` handlers recovered for the retired generated-code
product. None of them appears in any CMake target, and each includes
`import_stub.h`, which the break-first deletion removed. They are therefore
preserved evidence, not code: nothing in the product can reach them, and
nothing would fail if their behaviour were wrong.

The authenticated Gears 1 image declares 236 logical imports. Three are bound:
`XGetAVPack`, `XamInputGetState`, and `XamInputGetCapabilities`. Every other
import routes to the typed unsupported-service refusal, so the title cannot
execute past its first unbound service. This gap, not the dynarec, is what
stands between the current discriminator and S009 gameplay.

## Measured work list — 2026-09-22

`tools/import_inventory.py` joins the image's manifest to Xenia's ordinal
tables and to the recovered corpus. Against the ignored user XEX:

- 236 imports: 226 function, 10 variable.
- 0 unresolved ordinals. Every ordinal the image imports is declared by the
  vendored tables, which independently confirms the resolver the runtime
  compiles in against bytes it has never seen.
- 168 of the 226 function imports already have a recovered handler.
- 58 have none: 44 `xam.xex` and 14 `xboxkrnl.exe`. The XAM remainder is
  dominated by `NetDll_*` sockets, `XamShow*UI` blades, and voice/session
  services; the kernel remainder includes `NtQueryVirtualMemory`,
  `XexGetProcedureAddress`, `RtlUnwind`, and `__C_specific_handler`.
- 3 recovered handlers correspond to no import of this image, so the corpus was
  built for this title and is close to the right shape for it.

A recovered handler is preserved source, not a binding. `XamInputGetState` and
`XamInputGetCapabilities` are implemented in `x360port` and therefore appear as
uncovered in that count while being bound; the tool says so in its report.

## Required work

Re-own each service group over `x360port`'s typed import contract, claimed by
exported name through `x360port::ImportClaimTable`. The recovered handler
signature `(PPCContext&, uint8_t* base)` and its unchecked `base + address`
pointer arithmetic do not survive: `GuestImportContext` supplies the register
arguments, bounded guest reads and writes, a return value, and typed refusal.
The recovered *semantics* — status codes, measured constants, ordering, and the
evidence in the comments — are the part worth preserving, and each migrated
handler must be deleted from the orphaned file in the same change so neither
copy can drift.

## Do not revive `guest_heap.*`

`runtime/guest_heap.{h,cpp}` implements a page-granular guest allocator with a
coalescing free list, because the retired product had no guest memory owner.
Xenia's `Memory` and `BaseHeap` already own exactly these semantics —
`Alloc`, `AllocFixed`, `AllocRange`, `Decommit`, `Release`, and address-to-heap
lookup — and `x360port` embeds that `Memory`. Reviving `GuestHeap` would create
a second source of truth for guest allocation, and the two would disagree about
which pages are committed the first time the guest allocated through one and
freed through the other.

`NtAllocateVirtualMemory` and its siblings therefore need a title-neutral
`x360port` owner over Xenia's heaps, not a revived title-local allocator. That
owner does not yet exist and is the first prerequisite for the memory group.

The comments in `guest_heap.h` still record measured behaviour that the new
owner must satisfy: the guest's D3D resource destructor at `sub_82214C70`
chooses between physical and virtual free wrappers from a flag on the resource,
so a free must be routed by address rather than by the export that received it.
Preserve that finding when the file is removed.

## Ownership

Generic Xbox 360 kernel semantics — the 50 MHz Xenon timebase, FILETIME
encoding, virtual-memory status codes, synchronisation objects — belong in
`x360port`, not in `runtime/titles/gears1/`. Only bindings that encode a Gears
policy decision stay here. Splitting the corpus by that line is part of the
migration, not cleanup to do afterwards.
