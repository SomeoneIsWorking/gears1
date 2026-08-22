# GearsUE3 engine-port architecture

GearsUE3 is a clean-code, multi-title engine port built around static
recompilation and native overrides. It does not compile or redistribute Unreal
Engine 3 source. The user supplies an owned Gears disc/image; everything derived
from that image is generated locally into ignored storage.

## Product shape

One source tree produces a separate executable for each exact title revision.
Generated images cannot be linked together safely: they export the same guest
symbol namespace and compile against different image constants. Sharing happens
above that generated module, not by putting several games into one process.

```text
user-owned disc/image
        |
        v
local provisioner -> exact title/revision identity
        |             extraction, analysis, recompilation
        v
ignored generated title module
        |
        +---- factual title adapter: bindings, hashes, policies
        |
        v
shared GearsUE3 engine
  recomp ABI | Xbox services | RHI/render | audio | input | files | diagnostics
```

The tracked source owns algorithms and semantic operations. The locally
generated module owns recompiled functions, image contents, import tables, the
exact `TitleProfile`, and other title-derived artifacts. The shared profile
schema requires both container and parsed-image SHA-256 digests plus image base,
size, and entry point; malformed, duplicate, unknown, or ambiguous profiles
refuse before a title binding can activate. A tracked title adapter may contain
factual interoperability metadata, but never copied code, assets, decoded
instruction listings, or decompiler output.

## Ownership boundaries

| Owner | Shared | Per exact title/revision |
|---|---|---|
| Recomp | CPU ABI, direct/indirect dispatch, override registry, super-call | generated functions, image layout, entry point, function map |
| Xbox host | memory, kernel/XAM contracts, files, input, XMA/audio | genuinely observed title policy such as save namespace |
| Renderer | PM4/Xenos semantics, Vulkan resources, EDRAM/resolves, presentation | pass/hash bindings and title-only diagnostics |
| Native overrides | semantic implementations and runtime original/native selection | verified address binding and optional scope predicate |
| Tooling | disc/XEX readers, analyzers, deterministic provisioner | exact compatibility receipt and generated output |

Gears 1 currently supplies almost all runtime evidence. That does not make a
behavior Gears-1-specific when it is an Xbox contract. Split policy only when a
second title provides a discriminating counterexample.

## Override contract

Generated output is never edited. For every recompiled function, XenonRecomp
keeps the original body as `__imp__<name>` and emits a weak forwarding function
under the normal name. A strong title binding can therefore intercept direct,
same-translation-unit, cross-translation-unit, and indirect calls while retaining
an explicit super-call to the original body.

A production override declaration must eventually record:

- semantic operation ID;
- exact title/build identity and guest entry;
- observer, wrapper, faithful replacement, or enhancement classification;
- original body and native implementation;
- scope predicate and ABI/register/memory-write contract;
- evidence and provenance.

Faithful replacements stay runtime-toggleable in the same binary. The gate
compares guest-visible state and output between original and native arms and
contains a deliberately wrong control that the comparer must reject.

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

`GpuRetirement` is the tested owner intended for a bounded set of native frame
resource slots. It polls completion during normal progress and waits only for an
explicit teardown drain. It is not integrated into the Vulkan renderer yet.
That integration requires per-slot command buffers, fences or timeline values,
descriptor/arena/readback resources, and an explicit GPU-ready/consumer-complete
contract for shared scan-out images. `GpuQueueAccess` now serializes every shared
queue submission, presentation, queue-idle operation, and device-idle barrier as
Vulkan requires, but it does not provide those per-image dependencies. The
existing generation-tagged `RenderRetirement` remains the shipping
mechanism that delays guest `EVENT_WRITE_SHD` publication; the two types solve
different lifetime boundaries.

The required slot split is broader than command buffers and fences. Each slot
must own the draw, resolve, reinterpret, and depth-alias descriptor pools; the
arena and fallback allocations; readback; the mapped guest SSBO; and deferred
texture/upload cleanup. The gamma LUT buffer is also mutable GPU input and must
be image- or slot-local. Completion needs an autonomous pump because polling
only when another frame arrives can deadlock a title waiting for its retired
guest fence. Shared scan-out must publish a retained lease only after GPU
completion, and presenter submission must retain that lease until its own fence
retires. With two presenter frames in flight, retained-latest, recording, and
presenter-held lifetimes require four scan-out images in the worst case; the
current alternating pair cannot be reused safely by an overlapping renderer.

## Performance first, 60 fps override last

There are two independent limits. With rendering effectively disabled, Gears 1
still reaches only about 30 fps, so a title-side timing or frame-production
limit exists separately from renderer cost. A warm 743-draw gameplay frame also
costs roughly 44 ms, including about 34 ms in a flat draw-loop profile. Neither
changing the 60 Hz vblank source nor dropping stale presentation work proves
that the title simulates and produces frames at 60 Hz.

The immediate target is a stable, responsive Gears 1 at its faithful cadence.
Renderer lifetime fixes, native-engine performance, correctness, and glitch
prevention come before changing game timing. Compatibility reports still carry
a measured 60 fps enhancement result so absence cannot be hidden, but that
result does not block faithful compatibility readiness.

Once Gears 1 runs well enough and the compatibility/native paths are established,
the final per-game 60 fps enhancement has two required arms:

1. identify the semantic title timing limiter for each exact revision and add a
   runtime A/B enhancement that preserves the faithful 30 Hz arm; do not speed
   the general guest clock or patch an unexplained constant;
2. bring measured steady gameplay rendering below 16.67 ms per produced frame,
   with frame delivery and GPU retirement included in the measurement.

The first native boundary broad enough to change that result is a coherent
Xenon D3D/UE3 RHI frontend. It will mirror resource, state, shader, draw,
resolve, presentation, and retirement operations while initially super-calling
the recomp bodies. Only after its semantic draw stream and pixels agree with
the PM4 path may a runtime toggle bypass guest packet emission and parsing.

## Clean distribution and provisioning

The repository must build its clean tools and engine-owned tests without game
material. Its public history has been rewritten and the tracked-tip and
full-history distribution gates pass; those gates must be rerun after every
public-history change. A playable title target requires only a user-owned
disc/image.

`tools/title_identity.py` implements the first provisioning boundary. It
resolves an explicit path before `GEARS_ISO` and then an unambiguous ignored
`roms/` drop-in, streams the disc digest, records generic XGD/XEX identity when
available, and writes path-free JSON under ignored
`scratch/titles/<disc-sha256>/`. It does not yet extract, analyze, recompile, or
build a playable module. The complete provisioner must perform this operation
in a fresh, content-addressed directory:

1. resolve explicit CLI, `.env`, or a narrowly named ignored drop-in image;
2. safely extract and fingerprint the executable and required content;
3. select exactly one supported revision or refuse;
4. analyze helpers and switch tables without stale output;
5. recompile into ignored local source and build the title module;
6. write a receipt containing input, manifest, and tool revisions;
7. launch through the normal headless-capable product route.

Image base and size are not identity. Build selection requires exact container
and parsed-image digests because generated addresses and code describe the
parsed image while the container digest names the user's exact input. Unknown
or ambiguous inputs fail closed.

No UE3 source checkout, private repository, game download, pre-generated title
module, extracted asset pack, shader cache, or binary patch is an accepted input.
Title updates and DLC are separate copyrighted inputs unless they are present on
the user's supplied disc, so support claims must name the exact input scope.

## Compatibility claims

`tools/title_conformance.py` reports exact-build gates from local, digest-bound
evidence. It distinguishes identity, recompilation, headless boot, content
mount, menu, gameplay, compatibility rendering, native-renderer parity, and
override A/B results, plus a separately reported sustained 60 fps gameplay
enhancement gate.
Recognition is never promoted to compatibility, missing evidence fails, and
every referenced artifact must be relative, present, and digest-matched. For
Gears 2, Gears 3, and Judgment, the reporter rejects Xenia
as an oracle source; those titles need independent headless invariants,
original/native A/B evidence, or hardware-derived evidence.

| Title | Current evidence | Exact-profile/conformance gap | Oracle policy |
|---|---|---|---|
| Gears of War | Headless boot, menu, gameplay, compatibility renderer, and narrowly scoped renderer-oracle evidence exist | Current Gears 1 bindings have not yet been emitted through the exact `TitleProfile` boundary; native-RHI parity and an exact compatibility report are absent. The 60 fps override remains a deferred enhancement. | Xenia may support only the Gears 1 behaviors for which this project has separately validated the instrument |
| Gears of War 2 | None | No local exact revision, recompilation, content, gameplay, render, native-RHI, or 60 fps evidence | Xenia evidence is rejected |
| Gears of War 3 | None | No local exact revision, recompilation, content, gameplay, render, native-RHI, or 60 fps evidence | Xenia evidence is rejected |
| Gears of War: Judgment | None | No local exact revision, recompilation, content, gameplay, render, native-RHI, or 60 fps evidence | Xenia evidence is rejected |

“Uses UE3”, “is a Gears game”, and “the shared engine compiles” are not
compatibility results. Support is claimed for one exact executable revision and
one passed gate at a time.
