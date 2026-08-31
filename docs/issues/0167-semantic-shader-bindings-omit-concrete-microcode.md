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

Cross-reference against the independently verified device-reset adapter identifies the fourth captured
argument as the Gears 1 D3D-device global. The next diagnostic may therefore read that device's
post-call shader-object fields, but must still demonstrate a concrete module relationship before it
publishes a semantic binding.

The selected-call before/after capture falsifies this routine as the missing transition: its active
pixel and vertex shader objects are both unchanged while it emits the inline pixel module. It
serializes already-active device state, so it must not become a semantic publisher. The remaining
target is the earlier owner that established the active pixel object before this serialization.
