---
id: 136
title: Depth resolve component swizzles silently fell back to the unmapped R32 view
status: resolved
symptom: Gameplay replay warned that R32 depth resolve swizzle X111 could not be represented and silently served the unmapped R001 view
tags: render,depth,texture,swizzle,vulkan
created: 2026-08-24
updated: 2026-08-24
---

Root cause: TextureUploader::ResolveTargetView special-cased every depth resolve and returned the unmapped R32 view for any non-identity guest swizzle. Vulkan component mapping supports constants and missing components, so the title's X111 request is representable even though the image has one stored channel.

Fix: route depth resolves through the same cached component-mapped image-view path as color resolves. The current chapter-45 frame creates the X111 depth view without a warning or Vulkan validation error. Its 24 synchronous resolve handoffs remain matched; the current consumers read X, so this is a correctness fix for the other lanes rather than a claimed pixel change in this capture.

Falsifier: a validation error when creating an R32 component-mapped view, or a pass showing the X111 mapping does not return (R,1,1,1), invalidates this fix.
