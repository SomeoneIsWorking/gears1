---
id: 113
title: EDRAM depth alias shader declared RGBA16F while binding RGBA8 surfaces
status: resolved
symptom: Vulkan validation reported xe_surface storage-image format mismatches on populated SP_Prison_P gameplay frames; alias writes had undefined values
tags: render,gameplay,edram,depth-alias,vulkan,validation
created: 2026-08-21
updated: 2026-08-21
---

## Cause

The shipping depth-to-colour alias compute shader declared binding 2 as
`rgba16f`. `RenderTargetCache` deliberately preserves an unmixed
`k_8_8_8_8` surface as `VK_FORMAT_R8G8B8A8_UNORM`, then bound that view to the
same descriptor. Vulkan storage image formats are a shader/view contract, so
the alias loads or stores were undefined.

## Fix and proof

The shader now declares an unformatted write-only storage image and
`BuildDepthAliasPipeline` refuses devices without storage-image access without
format. `test_depth_alias_shader_format` parses the generated shipping SPIR-V
and requires its storage `OpTypeImage` format to be `Unknown`. A headless
SP_Prison_P HTTP probe rendered 1,159 draws at 1280×720 with the alias pipeline
active and `GEARS_DRAW_VALIDATE=1`;
`scratch/logs/probe_alias_fixed.log` contains zero validation errors, VUIDs, or
specification-information lines. The probe did not consume the deliberately
held 0/1 capture quota.
