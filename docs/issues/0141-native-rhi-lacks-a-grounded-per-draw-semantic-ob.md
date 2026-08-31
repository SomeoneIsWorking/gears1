---
id: 141
title: Native RHI lacks a grounded per-draw semantic observation seam
status: investigating
symptom: Per-draw semantics are now observed and packet-checked, but complete state, resource, resolve, presentation, and retirement semantics are not yet mirrored, so native execution cannot safely bypass PM4
tags: performance,native-rhi,d3d,re,draw,seam
created: 2026-08-27
updated: 2026-08-30
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

Resource reference operations are now part of the same ordered stream. The
retained `0x8222E868` AddRef and `0x8222E8E0` Release bodies grounded the object
flags, backing-resource link, big-endian atomic reference count, and the two
ownership boundaries. A shared native CAS owner executes non-boundary
transitions by default while the exact adapter keeps retained and alternating
A/B controls. A 12,000-call live A/B run matched every result and measured
roughly 51 ns native versus 72 ns retained mean execution; the longer
default-native run through frame 2940 matched 103,187 resource transitions with
no missing or mismatched result.

The gameplay-transition failure also exposed a missing state owner rather than
a draw defect. A binder-paused write watch attributed the direct stream-table
clear to `0x82487510`; its Gears 1 wrapper now emits one independently checked
16-slot reset event and the shared tracker applies the range clear. The
corrected transition run stayed free of RHI semantic errors beyond the old
first mismatch point, where the stale model had produced 154,333 draw
mismatches.

## Bound-state model progress

The native state tracker now retains every observed binding class needed by a
frontend snapshot: ordered texture slots, pixel and vertex shaders, index
buffer views, vertex streams, colour/depth targets, and target descriptors.
`ObserveRhiSemanticBinding` applies the post-call device-shadow evidence when it
exists, so a native consumer does not accidentally use a request-time
descriptor after the retained body normalizes it. Focused tests cover
post-call descriptor adoption and all four unbind paths. This is state-model
progress only; it does not enable a renderer bypass.

Static inspection also falsified the old resource-creation candidates
`0x82227000` and `0x82227120`: they index an existing object array and return
or update existing object state, rather than construct resources. Creation
entry points remain unidentified.

The next host-side prerequisite is now explicit in `runtime/native_rhi_resources.*`: a
title-neutral registry records construction identity and reference state, accepts only matching
non-boundary transitions, and refuses unknown objects, duplicate construction, mismatched metadata,
and release-to-zero destructor boundaries. The Gears 1 adapter decodes object flags, initial
reference count, and backing identity from the returned wrapper while retaining the raw five-word
audit. This is unit-tested resource bookkeeping, not live construction coverage or API allocation;
the zero-construction result in the frame-1440 walk remains unresolved.

## Next falsifier

2026-08-31 fresh exact-disc GCC headless menu observation reached frame 1,018
with 45,115 semantic matches, 5,519 missing joins, and zero value mismatches,
unkeyed renderer draws, or duplicate frames. Vertex and pixel microcode modules
matched exactly for all 47,357 observed shader pairs. This falsifies treating
the earlier zero-missing join reports as current-build parity: the missing
join population must be classified at the packet/materialization boundary
before it can support a native-bypass decision.

The existing command-buffer-transition scan was then applied to every normal
draw wrapper and retested through frame 1,032. It left the zero-key missing
population unchanged (46,477 matches, 5,710 missing, zero mismatches), so a
cross-buffer packet scan is not the cause. That speculative wiring was removed.
The next discriminator is a bounded census of the retained draw kinds whose
post-call packet evidence is absent; do not relabel those as renderer misses
until that producer-side evidence is classified.

Exercise the separate bound-vertex entry dynamically if the title reaches it.
Then add resource creation and live release-to-zero retirement effects and
compare shader modules/constants, texture content and backing-resource realization, remaining output
state, and pixels with the materialized renderer inputs. Normalized render/depth target state and
active six-dword texture fetch descriptors are now compared. The packet-keyed draw/index/target/
texture-descriptor comparison is still not full renderer-input parity. Keep the recompiled
compatibility bodies available and super-called until a deliberately wrong
semantic control is rejected and a same-run complete-stream and pixel-parity
gate agrees. Transient and bound-buffer agreement alone does not authorize a
native bypass.

The semantic resolve contract now retains the complete title-call payload that
is available at the grounded boundary: raw operation flags, source rectangle,
destination point, destination descriptor, and bytes per block. This removes a
lossy handoff before a native resolve backend is designed; it does not interpret
the raw flags or authorize backend execution. The packet comparer still proves
only the independently decoded address, pitch, height, and rectangle-list
submission.

The first longer live walk then exposed a false negative in this observer: the
retained body can cross its device command-buffer limit at `+0x30` and replace
the write-pointer buffer at `+0x28` during one resolve. At the failing event the
pointer moved from `0xa030eddc` to `0xa0017b28`; the old linear scanner treated
that valid allocation transition as an empty span. The bounded
`FindLastRhiDrawPacketAcrossCommandBuffers` path now retries the new buffer's
bounded tail only when the post-call pointer is lower, while preserving the
normal span and the existing packet-field checks. Focused regression coverage
exercises both paths. A Clang headless walk through frame 2280 then accepted
the previously failing resolve region with no semantic resolve refusal. This
repairs observation completeness; it does not provide a host backend or permit
the native frontend bypass.

The next 30-second identity-enabled Clang headless walk reached frame 703 and
matched 10,610/10,610 draw packets, 39,168/39,168 binding states,
5,103/5,103 resource references, 1,550/1,550 resolves, and 660/660 presents;
resource construction remained at zero. The title adapter now decodes the
already-grounded wrapper flags, type, backing object, and reference count once
for every binding state. `native_rhi_resources.*` can explicitly adopt that
identity and apply a non-boundary lifetime transition, but it still refuses
missing or conflicting identity evidence and does not allocate an API resource.
This closes the pre-existing-identity contract gap without claiming live
constructor coverage or native rendering.

The first isolated host operation is now grounded in
`runtime/native_rhi_vulkan_resolve.*`. It records a real `vkCmdCopyImage`
between backend-owned single-sample colour images, transitions their layouts,
and refuses unsupported operation flags, depth/stencil sources, multisample
images, mismatched formats, and out-of-bounds rectangles. Its headless Vulkan
test verified copied pixels and the unsupported-flags negative control. This
does not resolve the issue: the live plan still has no native resource
allocator, source-image producer, or same-binary state/pixel parity gate.

The host-side prerequisite is now separated into
`runtime/native_rhi_vulkan_resources.*`. It owns explicit Vulkan buffer and
colour-image allocations, stable native IDs, image layout state, and
retainable leases for fence-delayed destruction. The resolve test exercises
that owner, but no live constructor call has been observed and no native draw
producer consumes it. Wiring it to the semantic plan now would invent resource
meaning from absent title evidence, so the issue remains open.

## Renderer-input comparison

The old comparison was a false-positive seam: `runtime/vd_null_gpu.cpp`
published queued `FrameDrawItem` shape before the asynchronous renderer built
`NativeDrawInput`. It could not falsify a decode defect introduced by the real
renderer materialization path. That publication is removed.

`gpu_draw_native_input.*` now publishes one optional terminal record from the
shipping renderer attempt. Every source ordinal remains explicit as
materialized, refused, or resolve; renderer absence and latest-frame queue
replacement publish terminal frame outcomes. Observation-disabled runs do not
allocate the record vector. `rhi_renderer_input.*` owns the projection,
thread-safe two-sided join, bounded stale retirement, duplicate invalidation,
and reporting separately from semantic capture.

The first live join produced zero matches because it compared title CPU aliases
such as `0xA00D2384` directly with GPU physical packet addresses such as
`0x000D2384`. The existing `kGuestPhysicalAddressMask` contract names the root
cause. Correlation now uses `(guest-present sequence, canonical dword-aligned
physical packet address)`. Renderer executions sharing that key are checked as
predicated tile replays of one logical title draw; draw shape validates the
join but is never its identity. Repeated semantic keys, zero-key renderer
draws, extra renderer packet groups, missing sides, and duplicate publications
are explicit failures or coverage results.

A current Clang headless run through frame 300 observed 293 materialized
semantic matches, one explicit missing result from one replaced renderer frame,
zero value mismatches, zero unkeyed renderer draws, and zero duplicates. Thirty
renderer packet groups had no title-level semantic observation, including the
startup family; those are reported separately rather than being mislabeled as
semantic value mismatches. Focused controls cover altered index base, physical
alias normalization, matching and failing tile replays, repeated semantic-key
collision, zero-key input, semantic-first and renderer-first arrival,
pre/post-completion duplicates, history eviction, and both one-sided expiry
paths. Claim C102 and trusted instrument I067 record the live result and its
negative controls.

The unmatched population is now classified rather than left as an assumed
wrapper gap. The command processor carries the packet's source-buffer base and
ring-versus-indirect provenance through the terminal materialization record. A
headless run through frame 360 reported 30 cumulative unmatched packet groups:
24 refused and six materialized, all from indirect buffers, with no mixed tile
outcomes or source conflicts. The complete literal writer census for this exact
title revision shows that `0x8222B678` emits exactly the observed 24 point-list,
auto-index, one-vertex startup packets. The remaining six have the observed
rectangle-list auto-index shape emitted by `0x8222BC18` beneath the internal
clear chain rooted at `0x8222CC48`; none of that family has a normal semantic
draw wrapper. The movie content draw itself reaches the wrapped transient draw
path through `0x8221D3A8 -> 0x8222D4B0 -> 0x8222CFF8`. The reporter now also
classifies mixed-outcome tile replays and inconsistent source provenance as
their own packet-group results and prints `none` on cadence frames without a
current unmatched packet. Claim C103 records the ownership classification and
its falsifier.

This closes queued-versus-materialized draw-shape and index-address evidence.
It does not close issue #141: shader modules/constants, texture content and backing-resource
realization, vertex ranges, remaining viewport/scissor/output state, and pixels are not yet
compared. Internal initialization and clear draws
also remain outside the normal title semantic stream by design; their grounded
classification does not turn them into native frontend commands.

### Note (2026-08-30)
Renderer target-state progress: added one title-neutral decoder for RT0/depth base+format, signed color exponent, and surface pitch/sample state; the Gears 1 adapter now emits `0x82229B28` as a distinct ordered post-bind color-write transition, and the terminal compatibility projection compares only active semantic targets while treating backend attachment allocation as implementation detail. The first gameplay arm exposed a separate terminal-join defect: bound-index semantic addresses used a CPU alias while NativeDrawInput carried the physical DMA address. Canonicalizing both through `kGuestPhysicalAddressMask` removed every value mismatch. A Clang headless run through frame 660 then matched 10,945/10,945 correlated draws with zero missing/value mismatches and matched 15,306 color-write transitions, 2,854 color-target binds, and 1,566 depth-target binds. Field-specific controls reject every target field, missing state, unsupported MRT slots, and index-address changes. Texture fetch descriptors were still open at this point and are closed by the later note; texture content/resource realization, shaders/constants, remaining output state, pixels, and ownership of the larger gameplay unmatched-packet population remain open.

### Note (2026-08-30) — active texture-fetch descriptor parity

The first renderer-input texture arm carried only SetTexture bind-time descriptors and was rejected
66,429 times by frame 1172. Capturing the post-call fetch file from shader setters reduced the
failure to 325, consistently slot 0 dword 3 (`0x00000c14` semantic versus `0x00280c14` renderer).
A binder-paused descriptor write watch then identified the missing retained owners: `0x8222A150`
and `0x8222A2D8` update minimum and magnification filter state, `0x8222A550` updates anisotropy, and composite state block
`0x8254E9E0` calls SetTexture/filter setters and performs final direct descriptor writes. The exact
Gears 1 wrappers preserve and super-call all four bodies, capture their final device-shadow state,
and publish ordered texture-state mutations.

That investigation also fixed two defects in the write-watch instrument itself. ET_EXEC executables
have ELF load bias zero, so subtracting `dladdr().dli_fbase` had misidentified raw RIP `0xe962f1`;
the watch now resolves the containing PT_LOAD segment with `dl_iterate_phdr` and reports both raw RIP
and ELF address. Nested composite setters also require a pause depth: a Boolean paused state could
re-arm protected pages inside an outer retained call. Focused controls cover ET_EXEC, PIE, address
underflow, nested pauses, every one of the six descriptor dwords, missing state, duplicate and
unsupported slots.

The terminal compatibility projection now carries all 32 six-dword texture fetch descriptors but
compares only slots active in the semantic draw snapshot. A final uninstrumented Clang headless run
through frame 1607 produced 118,553 correlated semantic matches, zero missing joins, and zero value
mismatches. This closes active fetch-descriptor parity only. Texture content, backing-resource
realization, shader modules/constants, remaining output state, and pixels keep issue #141 open.

### Note (2026-08-31) — terminal join missing-reason census

The terminal renderer join now counts unkeyed semantic packets by draw kind and every concrete
missing renderer-evidence reason. A focused negative control supplies an unkeyed bound-index draw;
another supplies a renderer texture-state omission and verifies its reason is counted. This is the
shipping materialization boundary, not the older semantic-stream reporter that the live product does
not call.

On the ISO-derived Gears 1 Clang product, the bounded 35-second headless menu route reached frame
1028 with 46,185 semantic matches, 5,649 missing, and zero value mismatches. All four unkeyed
semantic-packet categories and unkeyed renderer draws remained zero. Every missing join was instead
`semantic-pixel-shader-missing`, while the renderer supplied exact pixel-shader module hashes for
48,479 correlated draws. Thus the gap is an ordered semantic pixel-binding publication, not packet
address canonicalization or renderer materialization. A module-only shader-flush state transition
was unit-tested but did not change the live census, so it was removed rather than retained as a
speculative fix. The next grounded step is to trace the ordered pixel-binding events at the title
adapter and identify which retained path clears or fails to publish the semantic state.

### Note (2026-08-31) — pixel-null setter is a marker, not the missing module

The title-neutral event stream now records whether a shader binding came from a setter or the shader
flush, and every draw snapshot retains its last effective pixel update for terminal attribution. A
Clang exact-disc headless route through frame 1032 observed 46,471 matches, 5,718 missing joins, and
zero value mismatches. Every missing logical draw's last pixel update was a zero-object setter;
there were 1,841 such setter clears, while 44,521 flush bindings produced no clears. This confirms
the setter clear is the immediate reason the semantic snapshot is absent, but not that preserving the
previous module is correct.

The required negative control made that distinction concrete. Treating the zero setter as an
observe-only event changed all missing joins into pixel-module mismatches: through frame 1030 it
produced 46,429 matches, zero missing joins, and 5,675 mismatches. The first retained semantic module
hash was `0xea0007942db096ad`, while the materialized renderer used
`0x63c971f5e9d59913`. That behavior experiment was removed. The next grounded step is to identify the
retained command path that selects the renderer's replacement pixel module after the zero setter and
publish its ordered module evidence; retaining the previous module would only hide the observation
gap.

The first candidate in that path was a genuine parser omission: the flush-range parser accepted
physical `IM_LOAD` packets but not inline `IM_LOAD_IMMEDIATE` packets, although the compatibility
command processor executes both. It now validates the inline payload length, stage, start, and
three-dword instruction alignment, converts the packet words to the same byte order used by the
renderer hash, and publishes its hash as module evidence. A focused immediate-pixel fixture proves
that path. The exact Clang run nevertheless remained at 46,957 matches, 5,756 missing joins, and
zero mismatches through frame 1037. Thus inline packet parsing is necessary observation coverage but
not the unobserved replacement module in this population. The next discriminator must observe the
global PM4 sequencer-load execution path that feeds renderer shader hashes, then correlate its
ordered pixel selections with title semantic frames; the single Gears flush wrapper is not enough.

### Note (2026-08-31) — global PM4 shader selections are the renderer source

`rhi_pm4_shader_evidence.*` now owns a bounded asynchronous join between immutable semantic frames
sealed at guest `VdSwap` and the command processor's `FrameDrawItem` shader selections captured when
that same swap packet executes. The payload is only canonical packet identity, provenance, and the
already-executed vertex/pixel FNV hashes; it cannot amend title state or depend on renderer
materialization. Focused controls cover semantic-first and PM4-first arrival, exact match, changed
pixel module, absent PM4 packet, and stale late evidence.

The exact-disc Clang headless route reached frame 1020. Its PM4 join reported 103 matches, 13 missing,
and zero mismatches for the frame, with no unkeyed packets; terminal renderer materialization reported
the same missing shape. Therefore the compatibility renderer is not inventing replacement hashes and
the remaining failure is an ordered semantic-publication gap. The next discriminator is to identify
the retained title command path that issues the PM4 replacement after the zero-object setter marker;
do not copy PM4 evidence back into semantic state, because that would hide the missing title boundary.

### Note (2026-08-31) — viewport candidate requires a clean database

A first lookup decompiled `0x8222ABF8` as a six-word viewport update calling `0x8222AB30`, whose
body writes packed scissor bounds. That result is not sufficient evidence for an implementation:
the existing Ghidra database intentionally replaces the save/restore-helper range containing
`0x828D2810` with `blr`, so the state-object return path is unavailable. The viewport setter remains
an unimplemented candidate until a clean or raw-disassembly-backed analysis establishes its object
provenance and live call frequency.
