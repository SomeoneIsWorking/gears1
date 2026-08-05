---
id: 76
title: Features(all=true) made every alpha-tested pixel shader an invalid module
status: resolved
symptom: 5 of 8 captures fail validation with VUID-VkShaderModuleCreateInfo-pCode-08740/08742, SPV_EXT_demote_to_helper_invocation declared but not enabled
tags: gpu,vulkan,validation,spirv,shaders,translator
created: 2026-08-05
updated: 2026-08-05
---

## Root cause

`MakeTranslator()` in runtime/gpu_draw_xlate.cpp passed `SpirvShaderTranslator::Features(/*all=*/true)`, which sets EVERY optional feature flag true. We enable none of the ones that require asking for. With `demote_to_helper_invocation` claimed, Xenia's translator emits pixel kill as `OpDemoteToHelperInvocationEXT` and declares both the capability and the `SPV_EXT_demote_to_helper_invocation` extension -- on a device that supports neither. Every alpha-tested pixel shader was therefore an invalid module. This driver executed them anyway; a stricter one refuses `vkCreateShaderModule` and those draws silently disappear.

## Fix

`features.demote_to_helper_invocation = false`, so the translator emits `OpKill`, which needs no feature.

**The better fix, NOT done:** enable the feature. `demote` keeps the invocation alive so neighbouring pixels' derivatives stay valid, which is exactly why Xenia prefers it over OpKill; with OpKill, mip selection near a killed pixel can differ. Enabling it means a `VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures` in the device pNext chain plus, below Vulkan 1.3, the device extension -- and the shared device may be created by gpu_draw.cpp (apiVersion 1.2) OR gpu_present.cpp (1.1), so both paths must change together or the adopting side gets a device without it. The reason is recorded at the call site.

## What Features(all=true) still over-claims

The other flags (fragment_shader_sample_interlock, the float-controls ones, image_view_format_swizzle...) are still claimed unconditionally. They produced no validation error on this device, but the same class of bug is latent in all of them. The honest shape is to build Features from `gears::DeviceCapabilities` -- runtime/gpu_device_features.{h,cpp} already computes what we actually enable, on both the creating and the adopting side.

## How this was nearly missed

The first pass ran the validator on ONE capture (act1_v2), which happens to contain no alpha-tested shader, and it came back clean. Five of the other seven were not. Validate across ALL captures -- `for c in scratch/frames/*.gfr` -- never one.

## Denominator for the fix

5 of courtyard's 71 translated pixel-shader modules contain a kill; they carry OpKill now, and all 8 captures hash identically to before the change.
