# GearsUE3 engine-port architecture

GearsUE3 is a clean-code native/dynarec engine for the Gears UE3 family. Native
owners replace selected subsystems; Xenia's existing x64/A64 Xenon dynarecs
execute every remaining guest path from the user's authenticated executable.
The gameplay product contains no interpreter, generated guest source, offline
translation step, or fallback to the former static product.

## Product shape

One source tree hosts exact title/revision adapters over one shared engine and
platform framework. The user executable remains runtime data; selecting a
different revision does not compile another source corpus.

```text
user-owned disc/image
        |
        v
local provisioner -> authenticated XEX/image + exact title/revision identity
        |
        v
x360port -> Xenia Memory / Processor / ThreadState / RawModule
        |     typed imports / device callbacks / runtime overrides / original calls
        v
x360ue3 -> versioned UE3 ABI / RHI semantics / engine lifetimes
        |
        v
GearsUE3 engine + factual title adapter
  native owners | RHI/render | audio | input | files | diagnostics
```

The tracked source owns algorithms, semantic operations, import declarations,
and factual title bindings. Runtime data owns executable bytes and mutable guest
state. The profile schema requires exact container and normalized-image identity
plus image base, size, entry point, and import facts before title policy
activates. Malformed, duplicate, unknown, or ambiguous profiles refuse. Useful
checked-image and typed-import validation facts currently prototyped in
`shared/xenon-host` move into `x360port`; its generated function-map contract
does not.

`x360ue3` is a clean, independently authored consumer of public `x360port`
interfaces. It owns reusable UE3-on-Xbox-360 ABI descriptions, RHI semantic
operations, title-supplied binding schemas, and object/resource/thread/frame
lifetime. It owns no Gears address, shader hash, pass roster, navigation, save
policy, gameplay rule, or application composition. Those belong to
`GearsUE3`. The local `shared/ue3` checkout is developer reference material
only and is never compiled, linked, packaged, copied, or required by a product.

Executable identity and save compatibility are separate policies. The current
Gears 1 profile retains the stable `gears1` save namespace across compatible
title updates; a future revision gets a different namespace only when its save
format or title identity actually requires isolation.

## Ownership boundaries

| Owner | Shared | Per exact title/revision |
|---|---|---|
| `x360port` executor | Xenia x64/A64 dynarecs and code cache; authenticated XEX mapping, typed imports, invalidation, bounded exits, runtime original/native/`super` selection | exact image identity, guest address bindings, and optional scope predicates are supplied as data by the consumer |
| `x360port` services/devices | guest memory and device callbacks, kernel/XAM contracts, files, controller devices, XMA, and raw Xenos boundaries | genuinely observed title policy such as save namespace and input action meaning remains in the consumer |
| `x360port` Xenos backend | PM4/Xenos semantics, Vulkan resources, EDRAM, resolves, and presentation primitives | no UE3 pass identity or Gears shader hash |
| `x360ue3` | versioned UE3 ABI descriptions, UE3 RHI semantic operations, engine object/resource/thread/frame lifetimes, and binding schemas | exact bindings and title-specific semantic exceptions are supplied by `GearsUE3` |
| `GearsUE3` | Gears-family native engine systems and reusable Gears behavior | exact title/revision addresses, pass/hash bindings, navigation, saves, enhancements, diagnostics, conformance, and application composition |
| Tooling | disc/XEX readers, deterministic provisioner, independent comparer | exact compatibility receipt and runtime profile |

Gears 1 currently supplies almost all runtime evidence. That does not make a
behavior Gears-1-specific when it is an Xbox contract. Split policy only when a
second title provides a discriminating counterexample.

## Override contract

The Xenia dispatcher consults an image-aware runtime override table for normal
calls. Disabled calls execute the original guest address through Xenia. Enabled
calls enter the native implementation. `super` suppresses only the current
override for one scoped call and executes the original guest body through Xenia,
then restores ordinary dispatch. Installing, removing, or changing an override
invalidates translated call paths that captured the old decision.

A production override declaration must eventually record:

- semantic operation ID;
- exact title/build identity and guest entry;
- observer, wrapper, faithful replacement, or enhancement classification;
- original body and native implementation;
- scope predicate and ABI/register/memory-write contract;
- evidence and provenance.

Faithful replacements stay runtime-toggleable in the same binary. The gate
compares guest-visible state and output between Xenia-original and native arms
and contains a deliberately wrong control that the comparer must reject. The
first Gears proof uses real leaf `0x8222E868`, typed `DbgPrint`, and all three
dispatch modes; it is not a boot or gameplay completion claim.

## Frame delivery and glitch prevention

The guest present number is the frame's identity. It must travel unchanged
through render submission, finished-image publication, scan-out, and
presentation; scan-out must not mint a second counter. Publication may skip a
dropped frame, and presentation may repeat the latest published image, but
neither may duplicate, regress to, or present an unpublished identity.

The render thread owns one active frame and at most one pending frame. A newer
submission replaces the pending frame instead of blocking the guest or making
the renderer work through stale images. This bounds presentation latency and
prevents an old completion from being mistaken for a newer frame. It does not
increase GPU throughput and is not evidence of a higher frame rate.

`GpuRetirement` now backs two native Vulkan frame slots. Each slot owns its
command buffer, fence, descriptors, arena, readback, and mapped guest SSBO; an
autonomous completion pump retires them in submission order. Ordinary live
frames return after CPU submission, while report/probe/replay frames explicitly
wait for their readback. `GpuQueueAccess` remains the shared queue's external-
synchronization owner. Generation-tagged `RenderRetirement` advances only from
the ordered GPU completion, so guest `EVENT_WRITE_SHD` publication cannot race
the memory its frame still reads.

Deferred texture/upload/fallback cleanup runs at the producer fence. Gamma LUT
storage is image-local. Shared scan-out has five retained images: publication
occurs only after producer completion, and each presenter slot retains its source
lease until the presenter fence retires. The capacity is derived from two
renderer slots, two presenter submissions, and the independently retained latest
publication, so recording can continue without overwriting an image still in
use. Device-idle transitions release presenter leases before teardown.

## Performance first, 60 fps override last

There are two independent limits. With rendering effectively disabled, Gears 1
still reaches only about 30 fps, so a title-side timing or frame-production
limit exists separately from renderer cost. Per-slot Vulkan timestamps now put
ordinary live title frames at 7-8 ms GPU and a captured 1,742-draw chapter-45
frame at 13.537 ms; the Release headless CPU path is 5-6 ms after removing an
unused full-frame readback. These measurements establish useful 60 Hz renderer
headroom for the bounded workloads, but not an 8.33 ms/120 fps budget or sustained
interactive-gameplay proof. The 8.33 ms target belongs to the native PC engine
path. The historical compatibility result is a migration baseline for Xenos
PM4, EDRAM, and pass reconstruction, not an accepted product ceiling. New
same-binary parity uses the Xenia-original guest path versus the native owner;
the frozen generated product is not a new oracle. Neither changing the 60 Hz vblank source nor
dropping stale presentation work proves that the title simulates and produces
frames at 60 Hz.

The immediate target is a stable, responsive Gears 1 at its faithful cadence.
Renderer lifetime fixes, native-engine performance, correctness, and glitch
prevention come before changing game timing. Compatibility reports still carry
a measured 60 fps enhancement result so absence cannot be hidden, but that
result does not block faithful compatibility readiness.

Once Gears 1 runs well enough and the Xenia-original/native paths are established,
the final per-game 60 fps enhancement has two required arms:

1. identify the semantic title timing limiter for each exact revision and add a
   runtime A/B enhancement that preserves the faithful 30 Hz arm; do not speed
   the general guest clock or patch an unexplained constant;
2. bring measured steady gameplay rendering below 16.67 ms per produced frame,
   with frame delivery and GPU retirement included in the measurement.

The first native boundary broad enough to change that result is a coherent
Xenon D3D/UE3 RHI frontend. It will mirror resource, state, shader, draw,
resolve, presentation, and retirement operations while initially calling the
original guest bodies through Xenia. Only after its semantic draw stream and
pixels agree with that Xenia path may a runtime toggle bypass guest packet
emission and parsing.

## Clean distribution and provisioning

The repository must build its clean tools and engine-owned tests without game
material. Its public history has been rewritten and the tracked-tip and
full-history distribution gates pass; those gates must be rerun after every
public-history change. A playable title target requires only a user-owned
disc/image.

The former provisioner proved strict input priority, bounded archive/GDF
extraction, exact container and normalized-image hashing, and fail-closed title
selection. Those facts are migration inputs; its XenonRecomp invocation and
generated-module build are retired behavior and must not be run. The replacement
provisioner must perform these operations in a fresh, content-addressed
directory:

1. resolve explicit CLI, `.env`, or a narrowly named ignored drop-in image/archive, materializing
   exactly one bounded disc-image member when the input is a supported 7z archive;
2. safely extract and fingerprint the executable and required content;
3. select exactly one supported revision or refuse;
4. load the authenticated executable through `x360port`/Xenia without emitting
   guest source or a precompiled title substrate;
5. write a receipt containing input, manifest, and tool revisions; and
6. launch through the normal headless-capable product route.

Image base and size are not identity. Runtime selection requires exact container
and parsed-image digests because title bindings describe one parsed image while
the container digest names the user's exact input. Unknown or ambiguous inputs
fail closed.

No UE3 source checkout, private repository, game download, pre-generated title
module, extracted asset pack, shader cache, or binary patch is an accepted input.
Title updates and DLC are separate copyrighted inputs unless they are present on
the user's supplied disc, so support claims must name the exact input scope.

## Compatibility claims

The replacement conformance report must distinguish exact identity, nonzero
Xenia JIT execution, gameplay-product interpreter absence, typed imports,
disabled/enabled/`super` overrides, relevant invalidation, headless boot,
content, menu, representative gameplay, renderer/native parity, and performance.
Recognition is never promoted to compatibility, missing evidence fails, and
every referenced artifact must be relative, present, and digest-matched. For
Gears 2, Gears 3, and Judgment, the reporter rejects Xenia
as an oracle source; those titles need independent headless invariants,
original/native A/B evidence, or hardware-derived evidence.

| Title | Current evidence | Exact-profile/conformance gap | Oracle policy |
|---|---|---|---|
| Gears of War | Historical static-path headless boot, menu, gameplay, compatibility-renderer, native-seam, and narrow renderer-oracle evidence | No x360port executor, x360ue3 integration, leaf/import/override discriminator, dynarec gameplay evidence, or complete conformance report exists | Xenia may support only the Gears 1 behaviors for which this project has separately validated the instrument |
| Gears of War 2 | Exact disc/XEX/image identity only | No runtime profile, x360port execution, x360ue3 integration, content, gameplay, render, native parity, or performance evidence | Xenia evidence is rejected |
| Gears of War 3 | Archive input present locally only | No verified exact revision or later gate | Xenia evidence is rejected |
| Gears of War: Judgment | None | No verified exact revision or later gate | Xenia evidence is rejected |

“Uses UE3”, “is a Gears game”, and “the shared engine compiles” are not
compatibility results. Support is claimed for one exact executable revision and
one passed gate at a time.
