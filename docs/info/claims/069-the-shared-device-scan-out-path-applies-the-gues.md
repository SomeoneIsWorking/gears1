---
id: C069
kind: claim
status: holds
created: 2026-08-20
tags: render,gamma,present
depends: runtime/gpu_scanout.cpp, runtime/gpu_scanout_gamma.cpp, runtime/scanout_gamma.cpp
---

## Claim

The shared-device scan-out path applies the guest display LUT exactly once before both presentation and requested readback

## Evidence

Live run logged 256 ramp writes with 254 non-linear entries and the GPU scan-out gamma pipeline; presented mean channel brightness was 17.4, matching the prior CPU-LUT measurement; test_scanout_gamma and clang-tidy pass

## What would falsify it

A shared-device capture differs from applying BuildScanoutGammaLut to the same pre-LUT bytes, or a probe observes a second LUT application
