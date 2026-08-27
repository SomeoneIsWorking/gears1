---
id: 141
title: Native RHI lacks a grounded per-draw semantic observation seam
status: investigating
symptom: Per-draw semantics are now observed and packet-checked, but complete state, resource, resolve, presentation, and retirement semantics are not yet mirrored, so native execution cannot safely bypass PM4
tags: performance,native-rhi,d3d,re,draw,seam
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

The first search assumed one title draw call should equal one compatibility-renderer draw execution. That assumption is false: the title emits one logical draw packet, then the Xenos command stream replays predicated packets per EDRAM tile. The inflated PM4 execution count hid the real normal draw family, while the prior static candidate `sub_82544148` was only a once-per-frame state operation.

## Resolution so far

A scoped alias-aware guest-memory write watch identified the packet producer
chain. Raw packet-construction scans and direct disassembly then grounded the
four normal entry points: `0x8222CFF8` (transient vertices), `0x8222D4F8`
(transient vertices and indices), `0x8222DA48` (bound vertices), and
`0x8222DE50` (bound indices).

The exact Gears 1 bindings in `runtime/titles/gears1/rhi_bindings.cpp`
retain and super-call every recompiled body. Only when
`GEARS_NATIVE_RHI_OBSERVE=1` is enabled, they publish ordered typed draws to the
title-neutral `runtime/rhi_semantic_stream.*`, which validates primitive type,
element count, and auto-index versus DMA source against the packet emitted by
the original body. A headless menu walk through frame 1712 observed 90,854
calls and produced 90,854 matches, zero missing packets, and zero mismatches:
17,864 transient-vertex, 60,465 transient-vertex-and-index, and 12,525
bound-index calls. The bound-vertex path remains statically grounded but was not
reached by that walk. A focused negative control mutates the packet evidence and
is rejected as a mismatch.

The same adapter now owns the already-grounded `SetTexture`, `SetPixelShader`,
and `SetVertexShader` addresses and device offsets. A headless run through frame
120 observed 734 texture, 118 pixel-shader, and 118 vertex-shader calls; all 970
requested objects matched the state written by the retained guest bodies, with
zero missing observations and zero mismatches. The stream now stores both
operation types in one typed event vector rather than separate queues, so the
cross-kind order needed by a native consumer is preserved directly. Focused
controls prove both the binding mismatch answer and interleaved draw/binding/draw
ordering. A post-refactor headless run through frame 60 matched 58/58 draw
packets and 490/490 binding updates with zero missing observations or mismatches.
`VdSwap` now appends a terminal present event to the same frame stream. The
extracted title-neutral `gpu_swap_packet.*` owner encodes and decodes the
compatibility transport, while the present comparer verifies its framing, frame
identity, front buffer, and six-word fetch description. A headless run through
frame 240 matched 240/240 present packets alongside 236/236 draws and 1,914/1,914
bindings, with no missing or mismatched observation. The historical shader argument scanner,
SetTexture census, and false `sub_82544148` draw probe were removed after their
findings were recorded. Remaining D3D submission/queue collection is explicitly
default-off, so ordinary runs no longer pay its atomics and table scans.

Selective inspection of the locally generated bodies recovered the transient
allocation ABI without committing generated code. The shared draw contract now
carries title-neutral guest-address/byte-range pairs. The Gears 1 adapter reads
the single-vertex mapping from the return register and the indexed mapping from
r10's output plus the PowerPC ABI's ninth stack argument; the existing
`guest_stack_argument` owner supplies that ABI rule. Expected byte counts come
from the public call arguments and are checked independently against the four
post-call device fields written by the retained bodies. A headless menu walk
through frame 1980 matched all 153,214 draws, including 28,550 transient-vertex
and 102,353 transient-indexed allocation pairs, with zero missing observations
or mismatches. Focused negative controls reject missing resource evidence,
wrong addresses, and wrong sizes.

The retained bodies also grounded `0x8222AFD8` as the index-buffer binder and
`0x8222B068` as the vertex-stream binder. Both are now ordered binding events.
Index-buffer objects are checked against device `+0x2F84`; vertex-stream
objects are checked against the table at `+0x2F88`, and their normalized fetch
words against the descriptor shadow beginning at `+0x2804`. The first live
attempt falsified the assumption that `object+0x1C` was copied verbatim: the
body conditionally rewrites four formats according to device mode. Capturing
the body result after the call and comparing it with the independent shadow
matches the actual setter contract. A fast headless run through frame 780
matched 3,924 index-buffer and 6,481 vertex-stream updates, with zero missing or
mismatched observations across 79,955 total bindings.

The bound index-buffer object is no longer opaque. Selective body inspection
identified common flags at `+0x00`, the GPU address at `+0x18`, and allocation
size at `+0x1C`; a live debugger sample demonstrated both 16-bit/endian-1 and
32-bit/endian-2 objects with plausible in-range allocations. The focused
`rhi_index_buffer.*` title adapter canonicalizes the address, decodes width and
endian mode, and refuses an out-of-allocation draw slice. The shared semantic
draw carries the complete allocation and consumed slice. Its comparer decodes
the retained body's DMA base, 16-bit-unit length, initiator width, and endian
mode independently. A subsequent headless run through frame 780 matched all
3,717 bound-index draws and 3,926 index-buffer bindings, with zero missing or
mismatched observations across 24,233 draws and 80,023 bindings. Focused
controls reject altered DMA addresses, widths, sizes, view fields, and missing
DMA evidence.

## Next falsifier

Exercise the bound-vertex draw path dynamically and recover complete vertex
buffer view data, then add resource creation/lifetime, resolve, and retirement events and
compare the complete ordered stream with the PM4-derived `FrameDrawInputs` and
output. Keep the recompiled compatibility bodies available and super-called
until a deliberately wrong semantic control is rejected and a same-run
complete-stream and pixel-parity gate agrees. Transient buffer agreement alone
does not authorize a native bypass.
