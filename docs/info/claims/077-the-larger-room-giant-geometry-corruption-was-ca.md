---
id: C077
kind: claim
status: holds
created: 2026-08-21
tags: render,synchronization
depends: runtime/vd_null_gpu.cpp#ExecutePacket, runtime/render_thread.cpp#WaitForRenderIdle
---

## Claim

The larger-room giant-geometry corruption was caused by EVENT_WRITE_SHD publishing GPU retirement while the native render thread still read guest frame memory

## Evidence

Catalog #118: pre-fix larger-room probe had 21,864 non-black pixels and giant cyan/magenta geometry while the exact captured frame replayed coherently in a fresh renderer; awaiting render completion before the fence produced a coherent 913,652-non-black-pixel larger-room frame at 6,809 draws with zero renderer drops.

## What would falsify it

A same-view headless control that waits for render completion before EVENT_WRITE_SHD reproduces giant geometry, or an immediate-retirement control remains coherent without resetting persistent renderer state.
