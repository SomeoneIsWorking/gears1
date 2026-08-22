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
generated module owns recompiled functions, image contents, import tables, and
other title-derived artifacts. A tracked title adapter may contain factual
interoperability metadata, but never copied code, assets, decoded instruction
listings, or decompiler output.

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

## Performance direction

The measured Gears 1 bottleneck is the PM4 compatibility rendering path, not
general PPC execution. With rendering effectively disabled the guest reaches
about 30 fps; a warm 743-draw gameplay frame costs roughly 44 ms, and its draw
loop has a flat profile rather than one dominant local hotspot.

The first native boundary broad enough to change that result is a coherent
Xenon D3D/UE3 RHI frontend. It will mirror resource, state, shader, draw,
resolve, presentation, and retirement operations while initially super-calling
the recomp bodies. Only after its semantic draw stream and pixels agree with
the PM4 path may a runtime toggle bypass guest packet emission and parsing.

## Clean distribution and provisioning

The repository must build its clean tools and engine-owned tests without game
material. A playable title target requires only a user-owned disc/image. The
provisioner must eventually perform the complete operation in a fresh,
content-addressed directory:

1. resolve explicit CLI, `.env`, or a narrowly named ignored drop-in image;
2. safely extract and fingerprint the executable and required content;
3. select exactly one supported revision or refuse;
4. analyze helpers and switch tables without stale output;
5. recompile into ignored local source and build the title module;
6. write a receipt containing input, manifest, and tool revisions;
7. launch through the normal headless-capable product route.

Image base and size are not identity. The compatibility gate must include a
digest of the parsed executable image because that is what addresses and
recompiled code describe. Unknown or ambiguous inputs fail closed.

No UE3 source checkout, private repository, game download, pre-generated title
module, extracted asset pack, shader cache, or binary patch is an accepted input.
Title updates and DLC are separate copyrighted inputs unless they are present on
the user's supplied disc, so support claims must name the exact input scope.

## Compatibility claims

Gears 1 is the only title currently verified. Gears 2, Gears 3, and Gears of
War: Judgment require independent revision identity, recompiler coverage,
headless boot/content gates, oracle validation, and gameplay/render evidence.
“Uses UE3” or “is a Gears game” is not a compatibility result.
