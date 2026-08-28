# Compatibility renderer and native RHI direction

The current GearsUE3 renderer executes the title's recompiled graphics path,
consumes the resulting Xbox 360 command stream, and reproduces the frame with
host Vulkan resources. It is the compatibility renderer, the behavioral oracle
for renderer migration, and the fallback that remains available after native
overrides land. The product architecture and distribution boundary are defined
in `docs/gearsue3-engine.md`.

## Current status

The compatibility path reaches Gears 1 gameplay and renders the deferred scene.
It has working command processing, shader translation, textures, render targets,
depth and stencil, resolves, scanout, presentation, frame capture, and headless
replay. Important remaining correctness gaps are tracked in the codemap,
re-frontier, and issue catalog rather than duplicated here.

The measured performance problem is architectural:

- with rendering effectively disabled, the guest reaches about 30 fps;
- a warm gameplay frame with roughly 743 draws costs about 44 ms to render;
- the draw loop has a flat profile rather than one dominant local shader or draw;
- the bounded asynchronous path reaches about 17-21 fps in the measured heavy
  phase, while a blocking retirement experiment collapsed it to 5 fps.

These measurements do not support another isolated shader optimization as the
primary performance plan. The cost includes guest rendering work, packet
emission, command parsing, register-state reconstruction, translation, and host
submission.

The hottest guest-side symbols in the steady gameplay profile are not native
rendering passes: `sub_8279B8C0` and `sub_8279DA90` expand and validate
XGraphics/Xenos shader IR, while the other sampled symbols are CPU data/setup
routines without PM4, draw, resolve, present, Vulkan, or RHI operations. The
native speed fix is therefore to stop invoking the compatibility shader/compiler
work in the native steady-state path, with native shader/material handling and
caching grounded separately; optimizing those compatibility helpers would not
produce the requested native engine.

## Native-pass seam

`runtime/native_pass.*` retains a compatibility seam for substituting an
independently authored host module for an observed pass. The clean tracked
roster now contains one implementation for the full-screen scene composite;
enabling `GEARS_NATIVE_PASSES=1` substitutes it only for its observed pixel
shader hash. That pass now supplies its descriptor, constant, sampler, and
interpolator contract directly, so its normal native arm does not invoke the
Xenos-to-SPIR-V translator. `GEARS_NATIVE_PASSES_KEEP_TRANSLATED=1` explicitly
retains translation for interface inspection and A/B debugging. The
compatibility arm remains available for the required same-input comparison.

The implementation is independently authored in
`runtime/shaders/native_scene_composite.frag` and its generated SPIR-V header.
The previous candidate's root cause was identified before restoration: it used
the translated `texture_swizzles` word as both channel routing and texture-sign
mode. The corrected module reads the fetch-zero sign byte from the separate
`texture_swizzled_signs` field; fetch routing remains owned by the host image
view, so the native shader does not apply `texture_swizzles` a second time. The
PWL gamma arithmetic was also corrected to add, rather than rescale, the
truncated correction term. A fresh signs-enabled same-input replay now matches
the compatibility arm to 0.0000 mean channel error, with no channel farther
than 4/255. The deliberately wrong-sign control diverged by 5.2155 mean
channel units and 66/255 worst difference, so issue #155 is resolved for this
pass and capture. This pass is parity-proven for the captured content, but it
does not establish the complete native renderer or its 5 ms budget.

A three-run sequential sweep of `title600.gfr` measured 5.574/7.305/7.411 ms
GPU on the compatibility arm and 5.276/5.709/7.157 ms with the native pass
enabled. The native log records the direct-interface path and no translation for
the implemented scene-composite pixel shader. The screenshots matched within
0.001 mean channel difference, with a 4/255 worst channel difference, and the
validation run exited cleanly without image-interface diagnostics. The retained
inspection control translated the same pixel shader and still matched within the
same tolerance. The sweep is directionally lower on average but too noisy to
claim a stable speedup, and neither arm establishes the native renderer's 5 ms
budget: all other draws still use the compatibility renderer, and the complete
PM4/RHI frontend bypass remains outstanding.

The six former title-derived shader substitutions are not shipping GearsUE3
implementations and are not evidence that a native renderer exists. Their
historical comparisons may explain past investigation, but no decoded shader
program, reconstructed formula, generated title module, or private-source
expression belongs in the tracked product or its documentation.

A future pass implementation must be independently authored from a public,
observable contract, keep the compatibility arm available, and pass a
discriminating same-input comparison. A declared identity, a generated SPIR-V
header, or a visually plausible frame is not implementation evidence.

## Native RHI boundary

The first native boundary broad enough to address the measured cost is a
cohesive D3D/RHI frontend. It owns resource creation and lifetime, state
dirtiness, shader selection, draws, resolves, presentation, and retirement as
one subsystem. Extracting only one setter or one pass would leave the dominant
packet-and-reconstruction architecture intact.

Migration proceeds in four ordered stages:

1. Observe semantic graphics operations while super-calling the retained recomp
   bodies.
2. Build a host-owned semantic stream and compare it with the compatibility
   renderer's derived frame inputs on identical captured state.
3. Reach state and pixel parity under a same-binary original/native toggle, with
   a deliberately wrong control that the comparer must reject.
4. Bypass guest packet emission and compatibility parsing only after that parity
   gate passes; retain the original arm for regression and title bring-up.

The shared frontend owns semantics. Exact executable addresses, revision
identity, and any factual pass binding belong to a locally generated title
module or a narrowly scoped title adapter. Gears 2, Gears 3, and Judgment require
their own headless evidence; Gears 1 results do not establish their compatibility.

## Verification

Renderer work is verified headlessly. Use deterministic frame captures and
offline replay for iteration, then compare the compatibility and native arms on
the same captured inputs. A valid gate must:

- refuse missing, empty, stale, or mismatched artifacts;
- verify interface and validation-layer correctness as well as pixels;
- identify the earliest state or output divergence rather than reduce a frame
  to one aggregate score;
- contain a negative control that proves the instrument can report a difference;
- report its title and exact revision scope.

Until the full native RHI parity gate exists, the PM4 path is the authoritative
renderer and native-RHI work remains observation-only.

## PM4-independent frame-plan boundary

`runtime/native_rhi.*` now converts an observed, evidence-checked semantic frame
into an ordered plan containing only draws, bindings, resource lifetime and
construction events, stream resets, resolves, and presentation. It is enabled
only with `GEARS_NATIVE_RHI_PLAN=1`, runs alongside the retained compatibility
renderer, and refuses incomplete evidence, non-monotonic ordering, or a missing
terminal present. A headless Gears 1 menu walk reached frame 1440 with every
reported frame accepted; later frames in a longer walk were refused when the
existing resolve observer reported missing or mismatched retained packet
evidence. This proves the seam can receive early complete ordered semantic
frames and correctly stop on a known parity gap, not that native rendering is
executing.

The plan deliberately has no PM4 packets, Xenos registers, EDRAM state, or
translated shader microcode. No host backend consumes it yet, and the retained
guest PM4 path remains authoritative. The next bounded candidate is the logical
resolve at `0x82235528`, but its existing host helpers still consume
compatibility `SurfaceTarget`/`ResolveTarget` objects. It therefore requires a
native resource/resolve contract, same-binary A/B output checks, PM4 absence
checks, and negative controls before it can become an execution arm.

## Native backend execution contract

`runtime/native_rhi_backend.*` now owns the narrow execution boundary after plan
construction. `ExecuteFrame` validates command ordering and terminal present
before calling a supplied backend, dispatches every semantic command in order,
and requires the backend to finish or cancel the transaction. A backend owns the
native resource map, pipeline/material selection, queue submission, and
retirement; the executor owns none of those and does not read PM4 state.

This is an interface milestone, not a renderer implementation. No backend is
installed in the product, so this change cannot accidentally replace the
compatibility renderer with a no-op. The focused native-RHI test exercises the
ordered dispatch, incomplete-frame refusal, command refusal cancellation, and
sequence rejection. A real backend still needs a title-neutral host resource
resolver and same-input state/pixel parity before it may be connected or used
for performance measurements. `runtime/native_rhi_resources.*` now supplies
the title-neutral guest-object identity and non-boundary lifetime part of that
resolver, with strict unknown-object and destructor-boundary refusal. API-
specific allocation and pipeline/material resolution remain unimplemented.
