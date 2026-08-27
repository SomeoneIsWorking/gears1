---
id: 143
title: Native shader setters are exact but slower than retained recomp
status: dead-end
symptom: Replacing SetPixelShader and SetVertexShader did not reduce frame cost and increased their call latency
tags: performance,rhi,native-override,shader
created: 2026-08-27
updated: 2026-08-27
---

## Finding

The exact-revision native implementations cover dirty masks, binding fields, patch records, vertex-mode state, safe fence stamping, and retained-body fallback for deferred queue insertion. A transactional write-set audit matched 240/240 eligible live calls with zero fallbacks.

## Performance result

After a 128-call warm-up, the same-run stage-balanced A/B measured 1273 ns native versus 1232 ns retained median over 176/176 calls. The title invokes these setters only about twice per frame. The native path is therefore opt-in and the retained recompiled path remains the default. This is useful parity infrastructure but not a performance optimization.
