# Project state

This ledger records independently observable capability coverage. Goals define why GearsUE3
exists, issues record atomic work, and the codemap records ownership; neither substitutes for this
done/partial/missing inventory.

| ID | Capability | State | Dependencies | Goals |
|---|---|---|---|---|
| S001 | Exact Gears 1 revision boots and reaches gameplay through the retained recomp path | verified | — | G001, G002 |
| S002 | Gears 1 compatibility renderer produces bounded, retired, headless gameplay frames | verified | S001 | G002, G003 |
| S003 | Native RHI semantic stream represents and checks the observed Gears 1 frame boundary | partial | S001, S002 | G001, G002, G003 |
| S004 | Grounded native RHI operations replace retained execution with same-binary controls | partial | S003 | G001, G002, G003 |
| S005 | Complete native RHI frontend bypasses title PM4 construction and compatibility reconstruction | missing | S003, S004 | G001, G002, G003 |
| S006 | Every Gears UE3 title passes an exact-revision compatibility gate | missing | S001, S002 | G001, G002 |
| S007 | Native engine sustains the 5 ms / 200 fps renderer budget on representative gameplay | missing | S005 | G003 |
| S008 | Public distribution provisions from a user-owned game image through `./run.sh` | partial | — | G004 |

## Current focus

S004 is the current focus: move measured, parity-checked RHI operations from retained bodies to
native owners while keeping runtime retained/A/B controls. The new title-boundary timing probe
rules out the startup Bink wait as the general gameplay 30 Hz limiter; a steady gameplay trace is
still missing.

## Capability details

### S001 — Exact Gears 1 retained execution

Evidence: The exact executable profile fails closed on container and normalized-image SHA-256,
boots the locally generated 191-translation-unit title module, and reaches the scripted gameplay
route headlessly. See `docs/re-frontier.md` (`title-revision-boundary`) and claim C080.

### S002 — Bounded Gears 1 compatibility rendering

Evidence: Two Vulkan frame slots carry submission, ordered fence completion, transient cleanup, and
scan-out publication. The validated gameplay run sustained 29.7–30.0 completed frames/s with zero
renderer-queue drops; focused tests cover the queue, frame identity, retirement, and cleanup owners.
See `docs/re-frontier.md` (`frame-delivery-contract`) and issues #139 and #140.

### S003 — Native RHI semantic observation

The observed Gears 1 boundary includes ordered texture/shader/buffer/target bindings, transient and
bound draws with resource views, device vertex/target state, logical resolves, presents, resource
reference transitions, resource-wrapper construction contracts, and the 16-slot vertex-stream reset.
Claims C096 and C097 record the native reference and reset-owner evidence.

Gap: The construction contracts at `0x8222EA18` and `0x8222EB78` are statically grounded but had
zero calls in the current headless walk through frame 1440; release-to-zero destruction is also not
covered live. The separate bound-vertex draw entry lacks dynamic coverage, and the complete stream
is not yet compared with renderer inputs and pixels. The new `native_rhi.*` plan boundary accepts the
complete ordered semantic stream on the same live path, but remains capture-only. See issues #141,
#148, #149, and #150.

### S004 — Native RHI execution

The shared big-endian atomic resource-reference owner now executes all observed non-boundary
AddRef/Release transitions natively by default. The exact Gears 1 adapter retains runtime recomp and
alternating A/B controls. Focused concurrent tests exercise the shipping atomic implementation; a
live 12,000-call A/B run matched every retained arithmetic observation and measured about 51 ns
native versus 72 ns retained mean cost (C096). The exact Gears 1 operation-kind-3 GPU ticket wait now
uses a notified packet-memory wait by default, while retaining the generated helper for progress,
hang, exemption, and A/B semantics; focused tests cover deadline arithmetic, address aliasing, and
state decoding. A headless current-build control reduced sampled process user CPU from 53.44 s on
the retained arm to 32.36 s on the native arm during 35 s runs while both stayed near 30 produced
frames/s. The Gears 1 audio mix kernel is now an opt-in native SIMD operation with a retained-body
super-call and 256 same-call audits; a current profile removed its former 18.59% CPU hotspot and
the warm gameplay render rate rose from roughly 15-16 to 24-30 frames/s on the measured path.
Existing shader and color-write setters remain executable native experiments but retained by
default because their timing gates showed no win.

Gap: Only the non-retiring reference-count fast path, the Gears 1 operation-kind-3 wait, and the
explicitly enabled Gears 1 audio mix are authorized. Zero-to-one backing ownership, one-to-zero
destruction, resource construction, draw submission, and renderer bypass remain on the retained
path. `native_rhi.*` now builds a PM4-independent ordered frame plan when explicitly enabled, but
does not execute a host backend or remove compatibility rendering. A candidate native scene
composite was rejected because its texture `kGamma` path failed same-input parity; the native-pass
roster is declarations-only again. The title still produces only about 30 frames/s, and the
complete native frontend has not been built. See issues #141, #149, #152, #153, #154, and #155.

### S005 — Complete native RHI frontend

Missing capability: No complete native frontend currently consumes the semantic stream, bypasses
title PM4 generation, and passes same-binary state plus pixel parity. The compatibility renderer is
still the shipping rendering authority.

### S006 — Multi-title compatibility

Missing capability: Gears 2, Gears 3, and Judgment have no locally proven exact revision,
provisioned module, headless gameplay evidence, or renderer compatibility/native parity report.
The conformance reporter exists, but no title has passed its complete report.

### S007 — Native 200 fps budget

Missing capability: There is no complete native renderer path to measure against the 5 ms / 200 fps
goal. Current compatibility frames are useful migration evidence, not proof of the native target;
the separate title-side approximately 30 Hz gameplay producer limit also remains unidentified.

### S008 — Clean image-only provisioning

Strict title identity, bounded GDF extraction, ignored content-addressed title storage, and clean
distribution gates exist.

Gap: Extraction, analysis metadata, recompilation, validation, build, and launch are not yet one
fresh-clone `./run.sh` flow. See `docs/re-frontier.md` (`rom-only-provisioning`).
