---
id: C071
kind: claim
status: holds
created: 2026-08-21
tags: gpu,vulkan,gameplay-scene
depends: runtime/gpu_draw_point_geometry.cpp#BuildPointGeometryShader, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl
reconfirmed: 2026-08-21
verified_at: 2026-08-21 12:25:41
---

## Claim

Chapter-45 point-list draws use a geometry stage instead of invalid bare Vulkan points.

## Evidence

Headless chapter45_recovered.gfr replay reports point_list x48, creates a 564-word point geometry shader, and tools/validate_all.sh reports none across its VUID set.

## What would falsify it

A gameplay capture containing point lists creates no point geometry module, raises topology-08773, or emits a point pipeline without a geometry stage.

## Re-confirmed 2026-08-21

Final Vulkan-validation run of walk_gameplay.gfr reports point_list x48 and builds the 564-word point geometry shader without topology-08773.
