---
id: C073
kind: claim
status: holds
created: 2026-08-21
tags: rendering
depends: runtime/shaders/edram_depth_alias.comp, runtime/gpu_draw_reinterpret.cpp#BuildDepthAliasPipeline
---

## Claim

The EDRAM depth-alias compute pass binds storage images without a declared format and is validation-clean on an RGBA8 gameplay surface.

## Evidence

test_depth_alias_shader_format passes against runtime/edram_depth_alias_spv.h; headless SP_Prison_P probe scratch/logs/probe_alias_fixed.log ran the depth-alias pipeline over 1,159 draws with GEARS_DRAW_VALIDATE=1 and zero validation findings.

## What would falsify it

The generated depth-alias SPIR-V declares a non-Unknown storage image format or Vulkan validation reports a storage-image format mismatch in this pass.
