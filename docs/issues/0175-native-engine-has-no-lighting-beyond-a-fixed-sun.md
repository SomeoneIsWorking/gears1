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

BSP light maps are decoded and drawn: `bsp::ModelComponent::Read` keeps each
element's 2D light map (three coefficient textures with RGB scales, then a
coordinate scale and bias), `bsp::BspModel` keeps each vertex's shadow texture
coordinate, triangulation writes it scaled and biased into texture coordinate
set 1, and `render::DrawMaterials` binds the coefficients as linear textures.
The shader weights each coefficient by 1/sqrt(3), the share an unperturbed
normal receives from each basis direction. Not yet rendered or checked on the
disc.

Next: static mesh components' light maps (their per-LOD data is not measured),
the material's normal-map input (so the coefficients are weighted by the
perturbed normal), and dynamic lights.
