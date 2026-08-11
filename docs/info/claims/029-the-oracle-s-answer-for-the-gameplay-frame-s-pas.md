---
id: C029
kind: claim
status: holds
created: 2026-08-11
tags: oracle,method
depends: extern/xenia
---

## Claim

The oracle's answer for the gameplay frame's pass outputs is robust across BOTH of Xenia's EDRAM models, so a pass where we differ from it is our defect, not its approximation

## Evidence

Re-ran the oracle with --render_target_path_vulkan=fsi (fragment shader interlock, EDRAM held as bits in software, Xenia's 'highest accuracy' path; this GPU advertises fragmentShaderSampleInterlock and fragmentShaderPixelInterlock) against the default fbo path, same content selector, same selected frame 3222. Mask copies: copy7 0.8469/83.5%-at-1.0 (fbo) against 0.7971/78.1% (fsi); copy10 0.9098/90.2% against 0.8967/89.2%. Scene copies: copy16 0.0189 against 0.0192, copy17 0.0247 against 0.0252. Both models agree on the kind of buffer every pass produces.

## What would falsify it

a pass where the two Xenia paths disagree materially -- then the oracle's value for THAT pass is model-dependent and cannot arbitrate
