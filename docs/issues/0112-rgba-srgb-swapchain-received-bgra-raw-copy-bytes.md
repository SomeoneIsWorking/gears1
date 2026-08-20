---
id: 112
title: RGBA sRGB swapchain received BGRA raw-copy bytes, exchanging red and blue
status: resolved
symptom: The presented Gears title screen is blue while the authoritative renderer frame is red
tags: gpu,present,vulkan,colour,srgb,swizzle
created: 2026-08-20
updated: 2026-08-20
---

Cause: gpu_present.cpp always created its raw-copy stage as VK_FORMAT_B8G8R8A8_UNORM. SDL selected VK_FORMAT_R8G8B8A8_SRGB (43), so vkCmdCopyImage preserved the BGRA stage bytes into an RGBA destination and exchanged red and blue. The prior headless present proof selected format 44 B8G8R8A8_UNORM and never exercised this branch. Fix: SwapchainSrgbStageFormat derives the matching UNORM component layout for every supported sRGB format, and SrgbRawCopyStage owns that resource. Evidence: the actual surface selected format 43, the runtime logged matching UNORM format 37, and the presented capture is red-dominant at mean RGB 49.091/9.220/8.818. test_swapchain_format pins RGBA, BGRA, packed A8B8G8R8, and rejection of non-sRGB formats.
