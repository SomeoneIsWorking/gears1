---
id: 167
title: Semantic shader bindings omit concrete microcode identity
status: investigating
symptom: Renderer materialization exposes executed vertex and pixel microcode hashes, but semantic draw state carries only opaque title shader-object pointers and therefore cannot compare shader identity
state_items: S003,S004
tags: native-rhi,shader,semantic-stream,renderer-parity,re
created: 2026-08-30
updated: 2026-08-31
---

## Root cause

The title-neutral semantic binding records only a guest object address. The compatibility renderer consumes concrete big-endian Xenos microcode bytes and identifies them with FNV-1a-64, so an opaque object cannot populate a native pipeline or falsify a renderer shader mismatch.

The first implementation decoded byte ranges from setter-time shader objects. A live headless negative result falsified that ownership: the startup renderer bound vertex hash `e8759fecd0f8e39c`, while the semantic object exposed only template candidates `5c6ecd0df751ec1f` and `ec32669a74fcae65`. The title patches vertex fetch state after setter time, so neither template candidate is the concrete bound program.

Retained function `0x822346A8` is the exact shader-state flush owner. It emits ordered Xenos `IM_LOAD` packets from the device's active pixel and vertex objects and may call `0x82221980` to roll to a new command buffer before continuing. Concrete microcode identity therefore belongs to the emitted packet span, not to the setter object layout.

## Required resolution

Capture the exact command spans produced by retained `0x822346A8`, including spans split by `0x82221980`; parse bounded `IM_LOAD` packets; hash the emitted physical microcode through the same title-neutral FNV-1a implementation as the renderer; and publish the final unpredicated load for each stage. Malformed or predicated evidence must clear prior identity rather than guess. Keep all device offsets and guest addresses in the Gears 1 adapter.

This implementation compiles in the shipping executable and has focused positive, zero-load,
malformed-packet, command-buffer-transition, exact-hash, mismatch, ambiguity, and inline-immediate
controls. The old setter-template decoder was removed. An exact-disc live observation now proves that
this one retained flush is incomplete for the current missing population: the global PM4 sequencer
selection carries the same concrete module hashes that the compatibility renderer receives, while the
semantic state remains absent after the zero-object setter marker.

## Current falsifier

A same-run headless observation must correlate global PM4 shader selections with every normal semantic
draw and show that the title adapter publishes the same concrete module before each draw. The current
falsifier is a nonzero PM4 pixel hash alongside an absent semantic pixel state; a deliberately changed
hash must remain a stage-specific mismatch, and malformed or predicated evidence must stay explicitly
missing rather than preserving a prior module.

## Packet-producer trace (2026-08-31)

The first missing replacement pixel module is inline rather than memory-backed, so its microcode has
no stable guest source address to watch. A bounded PM4-packet write watch instead armed the exact
inline packet and demonstrated one writer. Its host instruction is the retained generic copy helper
`0x828D2930`; a retained Gears 1 wrapper then established the relevant guest chain: the copy returns
inside command-buffer helper `0x8254F2B0`, which returns to live caller `0x8254E00C`. The helper
serializes the observed packet but is not itself evidence that a shader-object transition occurred.
The caller's Ghidra function boundary is not currently valid, so no semantic event was added. The
remaining discriminator is to establish that caller's ownership and prove a post-call module state
before publishing anything to the semantic stream.

The bounded guest-stack trace now places that helper below callback `0x82327E00`, which invokes the
larger retained packet routine before the serializer returns. Static reconstruction of that larger
routine ends at a title save/restore boundary, so this narrows the next dynamic target but still does
not prove a shader-object transition or authorize semantic publication.

The retained routine's selected-call ABI capture now confirms that the packet reaches it from
`0x82327E54` with six observed general-purpose arguments. Those arguments have not been identified as
a shader object or device state, so the observation establishes the callback handoff only. The next
discriminator must prove an argument's object identity before reading or publishing any of its fields
as semantic shader state.

Raw PPC reconstruction of `0x82327E00` establishes the actual call ABI: its outer callback object's
`+0x10` field is passed in `r3`, the content of global `0x82BECBA0` is passed in `r4`, and outer
fields `+0x20/+0x60/+0x64/+0x68` supply the remaining integer arguments. Neither fact alone proves
the ownership of any of those objects. The callback is a serializer over the `r3` command object, so
no state field may become a semantic binding until its relation to the emitted module is established.

The selected-call before/after capture shows the two sampled `+0x3080/+0x3084` fields are unchanged
while it emits the inline pixel module. That falsifies this routine as a direct mutation of those
fields, but it does not establish that they are the active semantic shader state. It must not become a
semantic publisher. The remaining target is the callback object's module-bearing state and its
earlier owner.

An opt-in pixel-object write watch now arms on `device+0x3080` immediately after a retained
zero-object pixel setter, while ignoring that known setter only on its calling thread. The first
implementation globally unprotected the watched page during known setters and faulted on unrelated
vblank state sharing guest page `0x4015e000`; the corrected thread-local scope kept the page
protected. Its selected packet reporter originally remained gated behind the independent packet
watch, so an object-watch-only run could not demonstrate that this serializer had run. It now runs
for either watch and emits a single explicit negative per arm. A bounded 25-second launcher run
reached frame 692, armed once at `0x4015e100`, reached the selected packet, emitted exactly one
no-unknown-writer result, and captured no target write. This falsifies a slot change before that
serializer edge only; it neither identifies an earlier unmodeled writer nor proves frame ordering
is the cause.

A current 25-second packet-watch control matched semantic and PM4 shader identity through frame 571;
the first 13 missing semantic draws appeared at frame 572, at the scene-transition boundary. The
watch still selected the known inline module, but its one-shot page watch armed after the packet had
already executed, so its lack of a later write attribution is not evidence about that first packet's
producer. Future instrumentation must select the relevant transition occurrence or observe the
callback object's module state directly.

That later recurrence has now occurred in the same bounded run: the selected packet was copied
through the retained helper chain and reached callback `0x82327E00` from `0x82327E54`. At that edge,
the outer callback passed its `+0x10` field as the serializer's `r3` command object and the sampled
global-device shader slots remained unchanged while the packet was serialized. This proves the
callback is an ordered serializer edge for the selected module, not a device-slot mutation. It still
does not identify a module-bearing field of the outer callback object or establish a semantic binding.
The next probe must snapshot only fields statically consumed by that outer callback object and
correlate them with both a selected-module positive and a non-selected negative before any
semantic-state change.

That selected-module snapshot is now complete. The serializer command object's directly consumed
fields `+0x4/+0x10/+0x1c/+0x28/+0x40` were unchanged around the copy; only `+0x28` was nonzero,
with value one. The outer callback supplied that command object through `+0x10`, a local parameter
block through `+0x20`, and its `+0x60/+0x64/+0x68` scalar arguments. This falsifies those directly
consumed command fields as a concrete-module source for this selected recurrence. It does not rule
out state owned above the callback or behind its parameter block, so neither snapshot may feed the
semantic stream. The next discriminator belongs at the earlier producer that constructs this outer
callback input, not at the serializer.

The live return address resolves that earlier relationship: the immediate caller loads a callback
object's vtable slot `+0x4` into CTR and dispatches indirectly to `0x82327E00`; it then calls the
same object's vtable slot `+0x0` to advance the iterator. The serializer callback is consequently a
dynamically selected method, not a direct static call site. The next grounded probe must attribute
construction or vtable installation for that callback object, rather than keep walking the caller's
linear control flow.

The selected recurrence identifies that callback vtable as `0x820E40B8`. At runtime its `+0x0` slot
is `0x8257A7A0` and `+0x4` dispatches the serializer at `0x82327E00`. The static Ghidra image has
zero words at that address and no code references to it, so it cannot recover the loaded table data.
That is a static-analysis limit, not evidence that the table is absent.

The image-load write watch initially missed this table because it classified only stores whose fault
address began within the watched word; the title-image copy uses an overlapping store. The corrected
watcher compares the word before and after each stepped same-page store, and its focused regression
exercises an eight-byte store spanning the target. A bounded headless run then captured one target
write at `0x820E40B8` while mapping the title image. The vtable is loaded title content, not a
runtime-constructed table. The next discriminator is the dynamic outer callback object that points to
it, rather than the static table itself.

Render-ring provenance now grounds that object: the selected callback object was exactly the start of
a 112-byte reservation returned to code at `0x82327D4C`. Raw PPC at that return writes the loaded
vtable pointer to the block start, copies a 96-byte stack payload into `object+0x10`, and then commits
the ring reservation. The callback is consequently rebuilt for each queued command, not a persistent
owner of shader state. Its construction payload is the next evidence boundary; no copied field may
become a semantic module binding until that stack payload's owner is identified.

Reconstructing the producer at `0x82327CA4` identifies that payload's shape. It copies one companion
word plus a 64-byte indexed record from its `r31` owner into the 96-byte local payload, invokes two
methods on that owner, then either calls the serializer inline or creates the 112-byte ring callback.
This is an ordered record-transport boundary, not proof that the record carries concrete shader
modules. The next target is the `r31` record owner's identity and record semantics; the semantic
stream remains intentionally unchanged.
