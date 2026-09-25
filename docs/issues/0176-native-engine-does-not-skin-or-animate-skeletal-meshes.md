---
id: 176
title: Native engine does not skin or animate skeletal meshes
status: open
symptom: the native player's body stands in its reference pose, and other characters, weapons and skeletal props are absent
tags: native-engine,mesh,animation
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

`mesh::SkeletalMesh` decodes the cooked SkeletalMesh layout measured on the
retail disc (Marcus, `COG_Marcus_Fenix_CamSkel`, 108 bones, 2 LODs). Every
byte of the export's native data is accounted for:

1. The bounds.
2. The material references.
3. A zero origin offset and the origin rotation (yaw -16384 on Marcus).
4. The reference skeleton, 48 bytes per bone: name, flags, quaternion, position, child count, parent.
5. The skeleton depth.
6. The LODs, each laid out as:
   - sections of 10 bytes: u16 material, u16 chunk, u32 first index, u16 triangles;
   - u16 indices;
   - empty shadow indices;
   - the active bone list;
   - empty shadow double-sided flags;
   - skin chunks: base vertex, two empty source-vertex arrays, bone map, rigid count, soft count, maximum influences;
   - size and vertex count;
   - empty edges;
   - required bones;
   - the raw point-index bulk data;
   - the skinned vertex buffer, 40 bytes per vertex: position, three packed tangent-basis vectors, UV, packed bone indices, packed weights.
7. The bone-name map.

The player's appearance is read from the pawn class defaults:
- The `Mesh` component names the skeletal mesh.
- `Default__Pawn_Infantry.WarPawnMesh` sets `Translation` to (0,0,-74).

`gears_native` draws Marcus's LOD 0 in its reference pose at the pawn, turned
to the pawn's facing.

Remaining work:
- The code is not built yet; the package census decodes every SkeletalMesh
  once it is built.
- Skinning: pose the bones and skin vertices by their chunk bone map.
- AnimSet/AnimSequence layouts. `Default__Pawn_Infantry.WarPawnMesh` names the
  AnimTree template and two AnimSets.
- Weapons, other characters, and skeletal level actors.
