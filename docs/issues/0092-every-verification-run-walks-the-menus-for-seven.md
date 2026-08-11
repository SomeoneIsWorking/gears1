---
id: 92
title: Every verification run walks the menus for seven minutes to reach a gameplay scene
status: open
symptom: the oracle comparison costs ~7 minutes per side because both emulators must boot and be driven through the menus into Act 1, and only the scene the walk ends at is reachable
tags: method,harness,ue3,re,oracle
created: 2026-08-11
updated: 2026-08-11
---

The paired capture (tools/layer_capture.sh) runs each side for up to 420 s,
driving a scripted pad route from the title screen into Act 1, because that is
the only way either emulator reaches gameplay. Two costs follow:

- every render change costs about fifteen minutes to judge against the console;
- only ONE scene is ever compared -- whatever the walk ends at. A defect that
  shows in a different level, or in a lit exterior rather than this cell
  interior, is not reachable at all.

The fix is not a faster walk. It is to reverse-engineer the engine's own map
load and ask for a scene directly. The chain is now RE'd and written up in
docs/ue3-runtime.md: GEngine at 0x82BED138, PrepareMapChange at 0x82426D98,
IsReadyForMapChange at 0x824272C0, ProcessAsyncLoading at 0x8242AFF8, and
FName::FName at 0x82364678, with the game's own call sequence read out of
sub_821B4620 at 0x821B4F0C..0x821B4F48.

Still open: the commit step is not identified, and the ORACLE side needs a
mechanism of its own -- Xenia cannot be told to call a guest function, so
whatever loads the map there has to be something the guest does by itself.
