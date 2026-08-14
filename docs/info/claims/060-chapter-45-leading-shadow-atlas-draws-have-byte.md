---
id: C060
kind: claim
status: holds
created: 2026-08-14
tags: render,oracle,shadow
depends: runtime/gpu_draw_vertexfetch.cpp#DumpVertices, runtime/gpu_draw_textures.cpp#TextureUploader::GetSampler, extern/xenia/src/xenia/gpu/vulkan/vulkan_command_processor.cc#VulkanCommandProcessor::IssueDraw
---

## Claim

Chapter-45 leading shadow-atlas draws have byte-identical guest geometry, pixel inputs, and translated shader modules on native and oracle; native sampler anisotropy now matches but does not resolve atlas sparsity

## Evidence

scratch/camerapair_chapter45_geometry_20260814; scratch/camerapair_chapter45_spirv_20260814; scratch/camerapair_chapter45_sampler_20260814; scratch/camerapair_chapter45_aniso_20260814; docs/issues/0109-chapter-45-outdoor-shadow-atlas-renders-mostly-e.md

## What would falsify it

A validated pair shows any selected draw with a differing complete index/fetch hash, relevant constants, translated SPIR-V, effective sampler state, or shows the anisotropy arm eliminating the first-atlas coverage gap
