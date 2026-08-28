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
independently authored host module for an observed pass. At the clean tracked
tip, the roster has declarations only: it contains no distributable implemented
module, and enabling `GEARS_NATIVE_PASSES=1` changes no rendering behavior.

An independently authored full-screen scene-composite candidate was exercised
against `scratch/frames/title600.gfr` and rejected. Its raw unsigned/signed
control was within 0.2621 channel units mean absolute error, but the first
guest-texture `kGamma` path diverged by 14.0665 under a forced `0x3f` sign word.
The candidate and its generated SPIR-V were removed; issue #155 records the
remaining sampled-view/sign contract investigation. No native-pass speed
measurement is valid until that parity gate passes with signs enabled.

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
