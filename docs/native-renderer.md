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

## Native-pass seam

`runtime/native_pass.*` retains a compatibility seam for substituting an
independently authored host module for an observed pass. At the clean tracked
tip, the roster has declarations only: it contains no distributable implemented
module, and enabling `GEARS_NATIVE_PASSES=1` changes no rendering behavior.

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
