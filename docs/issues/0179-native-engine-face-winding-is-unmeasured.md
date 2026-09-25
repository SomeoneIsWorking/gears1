---
id: 179
title: Native engine face winding is unmeasured
status: open
symptom: both faces of every triangle are drawn, so back faces and two-sidedness cannot be told apart
tags: native-engine,render
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

`render::MeshRenderer` uses `VK_CULL_MODE_NONE` because the cooked static-mesh
and BSP-fan windings have not been measured against a known outward normal.
Measure them (BSP node planes give the outward normal directly), then cull
back faces except for two-sided materials.
