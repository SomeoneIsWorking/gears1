---
id: 141
title: Native RHI lacks a grounded per-draw semantic observation seam
status: investigating
symptom: Per-draw semantics are now observed and packet-checked, but complete state, resource, resolve, presentation, and retirement semantics are not yet mirrored, so native execution cannot safely bypass PM4
tags: performance,native-rhi,d3d,re,draw,seam
created: 2026-08-27
updated: 2026-08-28
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

The retained bodies grounded `0x8222AFD8` as the index-buffer binder. The
initial identification of `0x8222B068` as a vertex-stream binder was wrong and
is now removed: its retained body writes one of four colour-target object slots
at device `+0x2F88` and an interleaved descriptor beginning at `+0x2804`.
`0x8222B398` writes the adjacent depth-target object slot at `+0x2F98` and its
two descriptor words. A live slot-zero object carried EDRAM base `0x2d0`, which
independently confirms render-target ownership. The first live colour arm also
falsified the assumption that `object+0x1C` was copied verbatim: the body
conditionally rewrites four formats according to device mode. Capturing the
body result after the call and comparing it with the independent shadow matches
the actual setter contract. A fast headless run through frame 780 matched 3,924
index-buffer, 6,481 colour-target, and 3,555 depth-target updates, with zero
missing or mismatched observations across 83,573 total bindings.

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

The actual vertex-stream binder is `0x8222AE20`. Its callers provide device,
slot, buffer object, byte offset, byte stride, and a dirty bit; the retained
body writes one of 16 object slots plus an offset-adjusted address, remaining
byte range, and quarter-stride shadow. The focused `rhi_vertex_buffer.*` title
adapter validates ranges and derives the expected view independently from the
call, while the binding observer decodes the post-call device state. A headless
run through frame 780 matched all 2,463 vertex-stream updates with zero missing
or mismatched observations across 86,002 bindings. Focused controls reject an
altered stride and out-of-range offsets.

The initial 18-slot interpretation was false. The retained reset loop increments
the slot from zero and exits when it reaches 16. The object table begins at
`+0x2F9C`, so its 16 entries end before the stride-byte table at `+0x2FE0`;
attempting to decode slot 17 reads four stride bytes as an object pointer. The
first bound-draw state comparison exposed exactly that phantom `0x09000000` or
`0x0A000000` object in slot 17.

The shared `rhi_semantic_state.*` owner now applies each vertex-stream binding
to an ordered slot map and snapshots the complete active views at each bound
draw. The Gears 1 adapter independently reads the proven 16 device slots after
the retained draw emits its packet. A subsequent headless run through frame
780 matched all 3,715 bound-index vertex-state snapshots, all 24,239 draw
packets, 86,061 bindings, and 780 presents; no observation was missing or
mismatched. Focused controls reject a changed vertex stride, missing state, and
different stream counts, and the state test proves slot ordering and unbinding.

Per-draw state also carries the ordered active color/depth target object
identities. A frame-600 headless run matched all 3,786 draw snapshots, including
482 bound-index draws, against independent reads of the four color slots and
depth slot; all 15,698 bindings and 600 presents matched too. The first arm
persisted bind-time descriptors and was rejected 3,141 times by frame 630 even
though target objects still agreed. In the repeated case the color descriptor
changed from `0x302D0` to `0xC02D0` without a target rebind, proving descriptor
state has a different setter owner. An alias-aware write watch first proved its
positive path by catching the retained target binder at host `0xE88B93`. The
corrected title adapter pauses the watch around that known binder; it then
caught retained guest function `0x82229B28` at host `0xE837CE` changing only
the slot-zero color descriptor. The body stores the requested mode, maps
surface-format pairs 2↔10 and 3↔12 in both the bound object and device shadow,
and marks dirty bit 37. A focused native implementation covers the exact state
transition and keeps the retained body callable. Its 256-call transactional
audit matched every owned byte and callee-saved register, including two live
format transitions. A clean alternating timing run rejected default native
execution: 40 ns native versus 30 ns retained median over 5,000/5,000 calls;
the retained body therefore remains the shipping path.

The logical resolve owner is `0x82235528`. Its Gears 1 adapter now decodes the
source colour/depth target, six-word destination texture descriptor, source
rectangle, destination point, physical address, pitch, and remaining height
before super-calling the retained body. It then inspects only the command span
emitted by that call and requires a three-element auto-index rectangle-list
copy whose post-call destination shadows agree. The first live arm exposed a
shared decoder defect rather than a title-offset defect: EXPAND formats 27, 28,
and 29 were absent from `ColorFormatBytesPerPixel`. After adding their 2/4/8-byte
sizes, a headless run through frame 540 matched 540/540 resolves with zero
missing or mismatched observations alongside every draw, binding, and present.

## Next falsifier

Exercise the separate bound-vertex entry dynamically if the title reaches it.
Then add resource creation/lifetime and retirement events and
compare the complete ordered stream with the PM4-derived `FrameDrawInputs` and
output. Keep the recompiled compatibility bodies available and super-called
until a deliberately wrong semantic control is rejected and a same-run
complete-stream and pixel-parity gate agrees. Transient and bound-buffer
agreement alone does not authorize a native bypass.
