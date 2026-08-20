---
id: C070
kind: claim
status: holds
created: 2026-08-20
tags: render,present,swizzle
depends: runtime/swapchain_format.h, runtime/gpu_present_stage.cpp
---

## Claim

An RGBA sRGB swapchain receives raw bytes through an RGBA UNORM stage rather than a BGRA stage

## Evidence

Actual SDL surface chose format 43, runtime chose matching stage format 37, and presented title capture was red-dominant at mean RGB 49.091/9.220/8.818; test_swapchain_format pins all mappings

## What would falsify it

A supported sRGB swapchain format maps to a UNORM format with a different component layout or the format-43 presented frame becomes blue-dominant
