---
id: 175
title: Native engine has no lighting beyond a fixed sun
status: open
symptom: every surface is shaded by one hard-coded sun direction; baked light maps, normal maps and specular are ignored
tags: native-engine,lighting,render
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

`bsp::ModelComponent::Read` validates and skips each element's 2D light map
(three texture references with RGB scales, then a coordinate scale and bias), and
static mesh components' light maps are not read. The BSP vertex pool's shadow
texture coordinates are skipped too.

Next: decode the light-map textures and coordinates for BSP and static meshes,
then the material's normal-map input.
