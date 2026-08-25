---
id: 108
title: Direct persistent-map boot reaches an incomplete later-level scene, then fades black
status: resolved
symptom: SP_EphyraStreets_P direct boot crosses the gameplay draw threshold but its exact front buffer has sky over a black lower half, the requested static-world camera shader is absent, and later frames become black
tags: oracle,capture,startup-map,checkpoint,gameplay-scene
created: 2026-08-14
updated: 2026-08-25
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

### Resolution (2026-08-25)
RESOLVED: the later chapter now loads through the title path on BOTH guests and the complete outdoor scene renders on each. Route: tools/startup_map.py checkpoint 45 swaps the shipped chapter45.sav into the campaign slot (byte-identical readback, original backed up); both emulators read that tree, and a walk of START then the campaign-menu sequence continues the campaign into sp_ephyrastreets_p. The first pair attempt failed on the oracle because the camera-pair short route (START~120@90, A@600) only opens the main menu - the A press selected CAMPAIGN and nothing navigated further, measured: the oracle sat at 192 draws/frame for 8400 frames. The working route adds the campaign-menu presses: 90:START~120 900:A 1050:B 1260:A 1500:A 1800:A 2250:A 2700:A 3150:A 3600:A. With it, the oracle reached chapter-45 gameplay and dumped the static-world camera constants (cb3cec323318973e, the c8..c11 view-projection per catalog #107) at guest frame 3901; our runtime, camera-gated on the frozen constants, matched at frame 3903 and captured the complete outdoor scene (frame_03903: Marcus in the street, full streamed world - the exact scene direct boot could never reach). Provenance MATCH, one camera digest. Structural pairing of the pair: the final composite matches at 0.00 percent of pixels differing, the light pass 0.90, all three bloom tiles under 1.25, depth agrees where both wrote; named renderer lead for the next step: two shadow-atlas resolves (srcD5A0 864x864 f22 #12 and #13) exist ONLY on our side in every one of the console's five dumped frames. Two harness defects were fixed on the way: camera_pair.sh validated native input and storage-selector lines with ^-anchored greps that predate lucent's timestamp prefixes, so the checks validated nothing while reading as one refusal (the pair itself was captured cleanly); and the UI-state check refuses this pair because the tutorial toast appears somewhere in the console's five-frame window but not at our captured frame - a one-frame-vs-window asymmetry for a transient overlay, left as the named gate for a full pixel pair rather than weakened. Evidence: scratch/camerapair_ch45_b/ (ours+theirs, PROVENANCE MATCH, layers/), scratch/camerapair_ch45_b.log, scratch/camerapair_ch45_20260825.log (the failed short-route attempt, kept as the negative).
