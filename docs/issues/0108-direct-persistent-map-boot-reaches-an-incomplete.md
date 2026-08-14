---
id: 108
title: Direct persistent-map boot reaches an incomplete later-level scene, then fades black
status: investigating
symptom: SP_EphyraStreets_P direct boot crosses the gameplay draw threshold but its exact front buffer has sky over a black lower half, the requested static-world camera shader is absent, and later frames become black
tags: oracle,capture,startup-map,checkpoint,gameplay-scene
created: 2026-08-14
updated: 2026-08-14
---

Measured on both direct-boot camera-pair attempts, not inferred from periodic screenshots. Oracle f583 resolves a non-black 1280x720 front buffer whose upper half is sky and whose lower half is near-black; it has 570 draws but no cb3cec323318973e static-world camera shader. At the ordinary +300 selector, f848 has only 62-64 draws and the later frame is black. camera_pair.sh correctly refuses both because the named camera shader is absent. Root cause boundary: loading a persistent map names package content but does not reconstruct the campaign checkpoint/Kismet state that streams and positions the complete level; treating the >=400 draw transition as proof of valid gameplay is false for this route. The title ships chapter45.sav through chapter47.sav for sp_ephyrastreets_p, and the verified menu/checkpoint path loads chapter saves. Next step is to select/load the later chapter through the title path on both guests, not weaken the content or camera gates.

### Checkpoint route established (2026-08-14)

The title's first campaign entry always consumed `chapter37.sav`; rewriting the
coalesced UI provider's `ChapterPointID` to 45 did not change that. The live
LoadChapter probe still populated a 385-byte carrier and resolved
`sp_prison_p`, so that config rewrite was discarded rather than shipped.

The working common seam is the checkpoint slot itself. `tools/startup_map.py
checkpoint 45` preserves `chapter37.sav` byte-for-byte, copies the shipped
`chapter45.sav` payload into that selected slot, parses its own serialized
header (version 2, chapter 45, `sp_ephyrastreets_p`), and verifies a
byte-identical readback. Both guests read the same extracted tree. On the real
native menu walk, the title's LoadChapter carrier became 444 bytes and the
checkpoint restore resolved `sp_ephyrastreets_p`. This reaches the complete
outdoor scene, including streamed world, player and tutorial Kismet state;
unlike raw persistent-map boot it retains the known static-world camera shader
and does not fade to black. `restore` restores both the coalesced config and the
original chapter-37 slot.
