---
id: 77
title: Act 1 rendering still has residual differences from the reference
status: open
symptom: live gameplay can differ in character and HUD content even though most structurally paired render passes agree
tags: render,oracle,gameplay-scene
created: 2026-08-05
updated: 2026-08-21
---

## Current status

The original list of five differences is obsolete. Many apparent divergences
were caused by mismatched game moments, different recording conventions, invalid
draw ordinals, or diagnostic tools that could not demonstrate their required
positive control.

The most consequential oracle defect was asynchronous first-use pipeline
creation in single-frame trace playback. Placeholder work contaminated raw pass
outputs before the real pipelines became available. Trace playback now creates
pipelines synchronously. With that correction and the later blur fix, all
structurally paired passes in the representative in-game frame agree under the
comparison threshold.

## Durable conclusions

- The former localization to one early scene draw is withdrawn. Its oracle
  checkpoint failed a forced-output positive control, so it could not prove what
  the target pass wrote.
- Shader, texture, constant, and geometry comparisons are useful only when both
  tools use identical field conventions and the same game state.
- Aggregate image scores do not identify the first faulty producer. Comparison
  must follow structural pass identity and stop at the first validated
  divergence.

## Remaining work

Residual exact-bit differences remain, and live captures have shown missing
character or HUD content. Those symptoms must be reproduced with shared input,
camera, UI state, and provenance before they can be attributed to rendering
rather than guest-state divergence. Keep the issue open until such a pair either
localizes a producer or shows the live symptoms no longer occur.
