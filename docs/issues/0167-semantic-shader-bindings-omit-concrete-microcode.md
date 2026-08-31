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

This implementation now compiles in the shipping executable and has focused positive, zero-load, malformed-packet, command-buffer-transition, exact-hash, mismatch, and ambiguity controls. The old setter-template decoder was removed. Live validation remains open because the required user-supplied title extraction is not currently available.

## Current falsifier

A live headless observation must show that flush-captured vertex and pixel hashes equal terminal renderer values for every compared draw, including startup hash `e8759fecd0f8e39c`. A deliberately changed hash must produce the stage-specific mismatch reason, and an injected malformed or predicated load must produce explicit missing evidence rather than preserve a prior module.
