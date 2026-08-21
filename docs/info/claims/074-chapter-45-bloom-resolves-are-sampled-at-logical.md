---
id: C074
kind: claim
status: holds
created: 2026-08-21
tags: gpu,bloom,oracle
depends: runtime/gpu_resolve_extent.cpp#FindResolveConsumerExtents
---

## Claim

Chapter-45 bloom resolves are sampled at logical 322x182 despite a 352x182 guest destination pitch; using pitch as Vulkan image width causes the first blur divergence

## Evidence

Headless exact-state replay: pre-fix bright pass matched but first/second blur had 1.36%/1.42% pixels over 0.1 and downstream C2D0 had 4.98%; after FindResolveConsumerExtents drives a 322x182 VkImage, all three C5A0 rows are 0.00% and C2D0 is 0.41%. Fetch constant base 0x006E4000 declares 322x182 and pitch 352. Unit test and layer_compare padding negative both pass; full validation run has zero VUIDs.

## What would falsify it

A synchronous-oracle replay of the same chapter-45 capture shows the bloom consumer uses another logical extent, or the old 352-wide image reproduces the fixed pass output
