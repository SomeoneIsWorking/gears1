---
id: 174
title: Native engine draws every material opaque
status: resolved
symptom: masked foliage and grates draw as solid cards; translucent glass, additive effects and modulative decals draw as opaque surfaces
tags: native-engine,material,render
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

## Resolution

`material::MaterialSurfaces` resolves each base material's `BlendMode`,
`TwoSided`, `LightingModel`, and `OpacityMaskClipValue`, and walks `OpacityMask`
(masked) or `Opacity` (translucent) to the texture and channel the nearest input
reads (its `Mask`/`MaskR`..`MaskA` flags). `render::TextureBindings` binds the
colour and opacity textures together; `render::MeshRenderer` has one pipeline
per blend, alpha-tests masked sections, and draws translucent, additive and
modulative sections after the opaque ones without depth writes. Unlit sections
skip the sun. SP_Adams_P with its 33 streamed sublevels draws 6,009 sections:
4,564 opaque, 1,252 masked, 40 translucent, 120 additive, 2 modulative.

Remaining: an opacity computed from constants or vertex colour is not evaluated,
so such a section keeps full opacity; blended sections are not depth-sorted.
