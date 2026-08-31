# Project state

## Comparison baseline

The baseline is the unmodified Xbox 360 release of *Gears of War* running on original hardware or
through Xenia, with its Xenos command stream, console services, and title-controlled 30 Hz rendering.
GearsUE3 intends a shared native PC engine for the series, with native semantic subsystems, exact
retained/native comparison, uncapped presentation, and user-owned-disc provisioning.

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
native owners while keeping runtime retained/A/B controls. The Vulkan resource owner now provides
explicit host buffer/colour-image allocation and fence-retainable leases, but it is not live-wired
because no constructor producer or native draw producer has been proven. The new title-boundary timing probe
rules out the startup Bink wait as the general gameplay 30 Hz limiter. Its post-Bink extension
shows ring reservations continuing after the traced producer stops while semantic presents remain
near 30 Hz; the steady gameplay producer or wait that limits those presents is still unidentified.

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
`native_rhi_resources.*` now provides a title-neutral guest-object registry for those construction
and non-boundary lifetime commands. Binding state and lifetime commands now carry an explicit
title-adapter-decoded object identity so a future host backend can adopt resources that predate the
observed frame; this is not API allocation. Claims C096 and C097 record the native reference and
reset-owner evidence.

Gap: The construction contracts at `0x8222EA18` and `0x8222EB78` are statically grounded but had
zero calls in the current headless walk through frame 1440; release-to-zero destruction is also not
covered live. The registry is unit-tested but has no live construction input and does not allocate
API resources. The separate bound-vertex draw entry lacks dynamic coverage. The stream now joins
each sealed semantic frame with the compatibility renderer's terminal `NativeDrawInput`
materialization result rather than its queued draw list. The bounded, duplicate-detecting join
correlates logical packets by guest-present sequence and canonical physical packet address, groups
predicated tile replays, and reports refused, dropped, unkeyed, unmatched, and value-mismatched
inputs separately. It compares primitive, count, indexed mode, index width/endian, and index-buffer
guest base. A current Clang headless run through frame 300 produced 293 semantic matches, one
explicit missing result for one replaced renderer frame, zero value mismatches, zero unkeyed
inputs, zero duplicates, and 30 renderer packet groups with no title-level semantic observation.
Command-buffer provenance now proves that all 30 groups came from indirect buffers. A complete
literal packet-writer census and the observed packet shapes classify 24 refused groups as the fixed
device-initialization point-draw batch at `0x8222B678` and the six materialized groups as internal
rectangle clears emitted below `0x8222BC18`; the four normal title draw wrappers cover the actual
content draws. Tile replays with mixed terminal outcomes and inconsistent source provenance are
reported explicitly instead of being classified from their first execution.
The shared target-state owner now normalizes active RT0/depth base and format, signed color
exponent, and surface pitch/sample state on both sides of that terminal join. The semantic tracker
applies `0x82229B28` as a distinct ordered post-bind color-write transition rather than pretending
it was a target rebind. A Clang headless gameplay-transition run through frame 660 matched 10,945
correlated semantic draws with zero missing or value-mismatched renderer inputs; the same interval
matched all 15,306 color-write transitions, 2,854 color-target binds, and 1,566 depth-target binds.
That run also exposed and fixed a separate renderer-evidence defect: bound-index guest CPU aliases
were compared directly with physical DMA addresses instead of through the existing physical-address
contract. Field-specific controls reject changed color/depth base and format, color exponent,
surface pitch/sample count, missing target state, and unsupported semantic MRT slots. A later
renderer-queue drop remains explicit missing coverage, and the much larger gameplay population of
unmatched internal compatibility packets remains outside normal title draw ownership; neither is
misreported as target-state agreement.
The terminal `NativeDrawInput` projection now carries the complete 32-slot, six-dword texture-fetch
register file. The semantic tracker applies ordered post-bind mutations from the grounded
`0x8222A150`, `0x8222A2D8`, `0x8222A550`, and `0x8254E9E0` retained bodies, while the renderer side
comes independently from the PM4 register file. Only active semantic texture slots are compared;
duplicate or unsupported slots, missing state, and every descriptor dword have explicit negative
controls and first-mismatch details. The initial bind-only model produced 66,429 mismatches, and a
shader-only post-state model reduced that to 325 before scoped write attribution identified the
remaining sampler-state owners. A final Clang headless run through frame 1607 matched 118,553
correlated semantic draws with zero missing or value-mismatched renderer inputs. This closes active
fetch-descriptor parity, not texture content, backing-resource realization, or sampled pixels.

Complete shader modules/constants, texture content/resource realization, remaining output state,
and pixels are not yet compared. The title viewport/scissor setter is grounded, but the bounded
gameplay join found a real semantic mismatch: the title-side state was `1280x720` while the guest
command stream programmed `PA_SC_WINDOW_SCISSOR_BR` as `1280x512`. The first-mismatch report now
preserves both states. Claims C102/C103/C104/C105 and instruments I067/I068 record this
materialization evidence. The resolve observer's false negative on
a retained command-buffer allocation transition is fixed with a bounded lower-pointer retry, and a headless walk
crossed the prior refusal region through frame 2280. The new `native_rhi.*` plan boundary accepts the
complete ordered semantic stream on the same live path, but remains capture-only. See issues #141,
#148, #149, and #150.

Shader-module comparison now has a compiled exact-evidence path: the Gears 1 adapter observes the
packet spans emitted by retained shader flush `0x822346A8`, follows any `0x82221980` command-buffer
rollover, and publishes the final bounded, unpredicated `IM_LOAD` payload per stage through the
title-neutral semantic contract. Setter-time object templates are no longer accepted as concrete
microcode. Focused controls cover positive, zero-load, malformed, rollover, mismatch, and ambiguous
evidence. Normal command-buffer submissions use the same parser at the grounded `0x822218C0`
boundary, including the selected 3327-dword submission at `0x7F740`; the command processor now
publishes the active modules at each executed guest draw packet, which is the stream the renderer
actually consumes. A bounded headless run reached that submission and, at frame 600, compared 111
semantic draws with 111 PM4 executions and zero missing/mismatched shader results; renderer
materialization compared 3,803 semantic draws with zero missing or mismatched results and 3,948
exact vertex/pixel module matches. Broader title/revision coverage and the native execution
milestone remain open under issue #167.

The compatibility renderer now consumes a single title-neutral `NativeDrawInput` produced by
`gpu_draw_native_input.*` instead of decoding the same register fields in its orchestration body.
`test_gpu_draw_native_input` covers refusal, target identity, 2X sample layout, output-merger state,
and host viewport/scissor derivation. This is an input handoff for a future native producer, not a
native execution claim.

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

The PM4-independent plan now also has an explicit `native_rhi_backend.*` execution contract. It
validates command ordering before dispatch, routes each semantic command to a supplied host backend,
and requires cancellation after a partial refusal. `native_rhi_resources.*` supplies that backend's
title-neutral resource identity and non-boundary lifetime bookkeeping, refusing unknown objects and
retained destructor boundaries. It now accepts explicitly supplied identity evidence for resources
whose constructors were not observed, while rejecting inconsistent metadata and zero-reference
adoption. The focused tests prove those transaction and registry rules; they do not provide a backend
or alter the shipping renderer.

The draw-input extraction removes a duplicate state-decoding owner and gives future native draw
execution one bounded input contract. Actual compatibility-renderer materialization now supplies
the parity side of that contract, including explicit refusal and queue-drop outcomes, but does not
authorize native draw execution or change the retained renderer's output.

The native-pass seam now contains independently authored scene-composite
vertex and pixel modules whose descriptor contracts validate as Vulkan 1.1
SPIR-V. Its first
candidate was rejected for conflating texture-sign and host-swizzle fields;
the corrected source separates those fields and its PWL gamma correction now
matches the translated arithmetic. The native arm now supplies the shared
`ShaderInterface` directly, skipping Xenos-to-SPIR-V translation for this
implemented pair, including the observed fetch-95 vertex layout;
`GEARS_NATIVE_PASSES_KEEP_TRANSLATED=1` retains the
translation arm for inspection. A fresh signs-enabled frame replay matched
the compatibility arm within 0.001 mean channel difference (worst channel
difference 4/255), and the validation run reported no image-interface
diagnostics. Three sequential four-repeat runs of `title600.gfr` measured
5.574/7.305/7.411 ms GPU retained versus 5.276/5.709/7.157 ms with the native
pass. The native values are directionally lower on average but the spread is
too noisy for a stable speedup claim. This is pass-local setup evidence, not a
complete native frontend or proof of the 5 ms target. See issues #155 and
#157.

Gap: Only the non-retiring reference-count fast path, the Gears 1 operation-kind-3 wait, the
explicitly enabled Gears 1 audio mix, and the one direct native scene-composite vertex/pixel arm are
authorized. Zero-to-one backing ownership, one-to-zero destruction, resource construction, the
remaining draw submissions, and renderer bypass remain on the retained path. `native_rhi.*` now
builds a PM4-independent ordered frame plan when explicitly enabled, and `native_rhi_backend.*`
plus `native_rhi_resources.*` define the host execution and resource-identity contracts. The actual
renderer-input join closes the former queued-input false positive, but it also confirms that the
live plan still lacks a native producer for those complete draw inputs and backend-owned source
images. The isolated `native_rhi_vulkan_resolve.*` slice now records a real host-owned Vulkan colour copy and
passes a headless pixel test with an unsupported-flags negative control. The companion
`native_rhi_vulkan_resources.*` owner now creates host buffers/images and retains them through
leases, but neither owner is connected to the live plan because native source-image production
and parity are missing. Compatibility
rendering remains authoritative. The native-pass roster remains experimental. The title still
produces only about 30 frames/s, and the complete native frontend has not been built. See issues
#141, #149, #152, #153, #154, #155, and #157.

### S005 — Complete native RHI frontend

Missing capability: No complete native frontend currently consumes the semantic stream, bypasses
title PM4 generation, and passes same-binary state plus pixel parity. A focused host-owned Vulkan
colour-copy operation exists, but it has no native producer or live resource/lifetime owner to feed
it. The compatibility renderer is still the shipping rendering authority.

### S006 — Multi-title compatibility

Missing capability: Gears 2 now has a locally verified disc/XEX/image identity, but it has no
title profile, provisioned module, headless gameplay evidence, or renderer compatibility/native
parity report. Gears 3 and Judgment have no locally proven exact revision or any of those later
gates. The conformance reporter exists, but no title has passed its complete report.

### S007 — Native 200 fps budget

Missing capability: There is no complete native renderer path to measure against the 5 ms / 200 fps
goal. Current compatibility frames are useful migration evidence, not proof of the native target;
the separate title-side approximately 30 Hz gameplay producer limit also remains unidentified.

### S008 — Clean image-only provisioning

Strict title identity, bounded GDF extraction, ignored content-addressed title storage, and clean
distribution gates now compose through the shipping `./run.sh` path. It accepts one user-owned image
or a 7z archive containing exactly one bounded disc-image member,
refuses missing/unknown input before build work, generates the local title module under
`scratch/titles/<disc-sha256>/`, and builds the product under `build/titles/`; its focused synthetic
tests cover the complete provisioning order and a Clang combined gate passed 101/101 checks. A real
user-owned Gears 1 XGD ISO then completed `./run.sh --headless --prepare`, verifying 1,810 extracted
files, the generated module, and the Clang product build; a subsequent bounded headless launch
selected the exact generated profile and produced retained-runtime frames without opening a window.

Gap: Only the exact Gears 1 profile is implemented. A user-owned Gears 2 archive now passes the
shared basic-compressed-XEX identity boundary (disc SHA-256
`1e76f91fa3bd804381c0ce5458bdb8dd2329c783b1be03b67d111136ce337230`, XEX SHA-256
`e98c25b4b9d173ac7ff69c84e7f1b4240c6be77c5fd7f672041a95221f68247c`, normalized image
SHA-256 `340f71de1cfaac7d98ff0478fc8c2954967aeb449f9e9916860592f7ee0b070c`), but it has no
supported profile or provisioned module. Gears 3 remains archive-only without a profile; Judgment
is not present. Additional-title conformance remains outstanding. See `docs/re-frontier.md`
(`rom-only-provisioning`).
