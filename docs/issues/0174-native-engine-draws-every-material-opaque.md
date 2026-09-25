---
id: 174
title: Native engine draws every material opaque
status: open
symptom: masked foliage and grates draw as solid cards; translucent glass, additive effects and modulative decals draw as opaque surfaces
tags: native-engine,material,render
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

`material::MaterialTextures` returns only a material's base-colour texture, and
`render::MeshRenderer` has one opaque pipeline. The base material's `BlendMode`,
`OpacityMaskClipValue`, `TwoSided`, and the texture and channel its `OpacityMask`
or `Opacity` input reads are not carried to the draw.

Next: resolve each material's surface (blend mode, opacity source and channel,
clip value), bind the opacity texture beside the colour texture, alpha-test masked
sections, and draw translucent, additive and modulative sections after opaque ones
with their blend state and without depth writes.
