---
id: 92
title: Every verification run walks the menus for seven minutes to reach a gameplay scene
status: open
symptom: the oracle comparison costs ~7 minutes per side because both emulators must boot and be driven through the menus into Act 1, and only the scene the walk ends at is reachable
tags: method,harness,ue3,re,oracle
created: 2026-08-11
updated: 2026-08-12
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

### Note (2026-08-12)
A RUN CAN FAIL WITH THE GPU BACKEND NEVER INITIALISING, AND THE LOG SAYS SO IF YOU KNOW WHERE TO LOOK. An oracle run intended to measure per-draw primitive counts produced 'STOPPED at guest frame 0 waiting for frame 1200 -- the title stopped presenting. 0 of 0 captures', with the log ending at guest thread creation after 442 s. That reads like the slow-boot variance this issue is about and it is NOT: NONE of the GEARS knobs that VulkanCommandProcessor::SetupContext logs appeared -- not GEARS_ORACLE_PRIM_STATS ('will count assembled and post-clip primitives...'), not GEARS_ORACLE_DUMP_AFTER_GAMEPLAY ('resolve dump waits N further frames'), not GEARS_ORACLE_DUMP_FRAMES ('dumping N consecutive frames'). Only the app-level oracle lines logged. Since every one of those is read in the same SetupContext block, their absence means the command processor never reached it: THE GPU BACKEND NEVER INITIALISED, which is why there were no presents. THE DISCRIMINATOR IS CHEAP AND WORTH KEEPING: grep the oracle log for any SetupContext knob line. Present means the GPU came up and the title really is slow (this issue); absent means the backend never started and the run says nothing about boot time at all. I nearly filed this as another instance of boot variance. ENVIRONMENT AT THE TIME, checked rather than assumed: gpuguard latch clear, 0 amdgpu trouble lines in a 30-minute kernel window, no DEVICE_LOST anywhere in the log, VRAM 84.9% free -- so this was not a fault of ours and not a card reset. Another session was running its own GPU work on the same machine, which makes transient contention during device acquisition the most likely cause. ALSO NOTED, though not the cause here: scratch/vsord/run.sh passes --oracle_frame_timeout equal to its own wait budget, so the oracle gives up at the same instant the wrapper would, and 'the oracle timed out' cannot be told from 'the wrapper stopped waiting'.

### Note (2026-08-12)
A SECOND FAILURE MODE, DISTINGUISHED FROM THE FIRST BY THE DISCRIMINATOR ADDED EARLIER TODAY. A paired capture presented ONE frame carrying ONE draw and then stopped ('1 frames presented, none with >= 400 draws yet (busiest so far: 1 draws)'), dumping 0 of 40 window frames in 600 s before the script refused. Applying this issue's own test: FIVE SetupContext knob lines are present in the oracle log, so unlike the failure recorded above the GPU backend DID initialise, and no DEVICE_LOST appears anywhere. So this is a guest-side stall at boot rather than a backend that never started, and the two are now separable from the log alone. THE LIKELY CAUSE IS MACHINE CONTENTION AND IT IS MEASURED, not assumed: load average 13.04 / 14.13 / 14.59 at the time, with sibling sessions launching their own GPU processes twice in the preceding two minutes. The oracle has to JIT and run a guest, and CPU starvation at that load plausibly leaves it unable to reach its first real frame. Two of three oracle launches in this window failed this way while a short 100 s one succeeded. THE OPERATIONAL RULE THAT FOLLOWS: check  16:56:00 up 15:21,  1 user,  load average: 9.96, 12.88, 14.12 before committing to a ten-minute oracle run on this machine. A run launched into load 13 costs the full timeout and returns nothing, and repeating it is the grinding the workflow rules forbid rather than a diagnosis.
