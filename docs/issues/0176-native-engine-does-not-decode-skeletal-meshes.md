---
id: 176
title: Native engine does not decode skeletal meshes
status: open
symptom: characters, weapons and skeletal props are absent from native renders
tags: native-engine,mesh,animation
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

Only `StaticMesh` and BSP geometry are decoded. SkeletalMesh LODs, skin weights,
the reference skeleton, and AnimSet/AnimSequence data need layouts measured from
the disc, as the static mesh layout was.
