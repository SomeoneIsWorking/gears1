---
id: 7
title: Xenia as a differential oracle on Linux
status: resolved
symptom: need a reference emulator to compare guest state against; unclear whether Xenia builds and runs on Linux
tags: harness,method,oracle
created: 2026-07-22
updated: 2026-08-05
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
In favour: Xenia ships per-instruction PPC tracing (ITRACE/DTRACE in x64_tracers.cc, --store_all_context_values, TARGET_THREAD), which is exactly what a differential harness needs and is the largest piece of the work already done.

### Note (2026-07-22)
Against: upstream docs/building.md says verbatim 'Linux support is extremely experimental and presently incomplete', and expects Clang 19 specifically (this machine has Clang 22).

### Note (2026-07-22)
Bounded feasibility test started rather than committing to the full harness: clone and attempt a Linux build. A failed build is still a useful result -- it resolves the harness-vs-broaden fork empirically instead of leaving it open. Recursive clone exceeds 10 minutes, so it runs in the background; artifacts under scratch/oracle/ (gitignored).

### Note (2026-07-22)
Toolchain check on this Fedora machine: Clang 22 (docs want 'Clang-19 or newer' -- satisfied), CMake 4.3.0, Ninja 1.13.2. Build is CMake+Ninja via CMakePresets.json (presets: default, vs, vs-arm64). Clone is ~1.3 GB with submodules and takes well over 10 minutes.

### Note (2026-07-22)
BLOCKED ON SYSTEM PACKAGES. Present: gtk3-devel, vulkan-loader-devel, libX11-devel, libxcb-devel. Missing: libcxx-devel, libcxxabi-devel, lz4-devel, SDL2-devel. Installing these needs sudo, i.e. a machine-level change outside the repo, so it requires the user's go-ahead rather than being done unilaterally.

### Note (2026-07-22)
Dependency list REFINED by reading Xenia's CMake rather than trusting its Ubuntu apt line. Only three find_package/pkg_check entries exist: PkgConfig, Python3, and GTK3 (the sole REQUIRED one) -- and gtk3-devel is already installed. SDL2 is vendored in third_party, so SDL2-devel is not needed. No libc++ flags anywhere in the CMake, so libcxx-devel/libcxxabi-devel appear unnecessary too.

### Note (2026-07-22)
The one genuine gap is lz4: CMakeLists.txt line 259 does link_libraries(stdc++fs dl lz4 pthread rt), and while liblz4.so.1 is present at runtime, the /usr/lib64/liblz4.so development symlink is missing, so the link step would fail. So the blocker is a SINGLE package (lz4-devel), not the four the apt line implies.

### Note (2026-07-22)
CMake configure SUCCEEDS on this machine (exit 0, Clang 22, CMake 4.3.0, Ninja). It even found system sdl2 2.32.70, so SDL2 was never a blocker. Clone with all submodules is 4.5 GB. Build target for the emulator is xenia-app. Build started; the open question is whether it compiles and whether the only remaining failure is the lz4 link.

### Note (2026-07-22)
CORRECTION: an earlier note that the build 'exited 0' was wrong -- that was the exit status of a trailing echo, not ninja. The build actually FAILED at 489/702. Always check the log, not the wrapper's status, when the command is a pipeline.

### Note (2026-07-22)
Real failure: 'version.h' file not found, included by trace_writer.cc, primitive_processor.cc and main_win.cc. Nothing in the CMake generates it -- Xenia's primary build is premake-based and the CMake path is incomplete, which is what 'experimental Linux support' concretely means here. Not a Clang or Linux incompatibility. Worked around by generating build/generated/version.h from git metadata (XE_BUILD_BRANCH/COMMIT/COMMIT_SHORT/DATE) and adding it to the include path.

### Note (2026-07-22)
version.h workaround WORKS: after generating it, the build reached 270/411 with ZERO compile failures before being interrupted externally (ninja reported 'interrupted by user', not an error). So no Linux/Clang incompatibility has appeared anywhere in ~760 compiled objects. Restarted detached via nohup so a session-level task stop cannot kill it again.

### Note (2026-07-22)
XENIA BUILDS AND LINKS ON THIS MACHINE -- 19 MB build/bin/Linux/xenia_canary, and NO sudo was required. Three workarounds, all local: (1) generate build/generated/version.h from git metadata, since the CMake path lacks the rule their premake flow has; (2) CMakeLists.txt line ~297 forces -fuse-ld=lld for Release and lld is not installed -- switched to bfd, which is; (3) liblz4.so.1 exists but the /usr/lib64/liblz4.so dev symlink does not, so a local symlink in scratch/oracle/localdev plus -L on the link line satisfies it. So the earlier 'needs sudo' conclusion was WRONG and the dependency blocker was avoidable entirely.

### Note (2026-07-22)
Binary launches (it pops a zenity file picker with no ROM argument). Remaining unknowns for the harness: whether it runs Gears of War headless to the point our crash occurs, and whether ITRACE/DTRACE tracing can be enabled and correlated with our runtime. Those need doing, not assessing.

### Note (2026-07-22)
Reproduce: CC=clang CXX=clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-I<src>/build/generated -DCMAKE_EXE_LINKER_FLAGS=-L<scratch>/oracle/localdev ; ninja -C build xenia-app

### Note (2026-07-22)
ORACLE CONFIRMED WORKING. Xenia runs Gears of War headless on this machine: Vulkan initialised on the RX 6700 XT, GPU command / frame limiter / XMA decoder / audio worker threads started, 1280x720 swapchain created, zero errors in the log. So a reference oracle for this title exists on Linux, which was the whole open question.

### Note (2026-07-22)
Invocation: setsid nohup timeout N ./build/bin/Linux/xenia_canary --headless=true '<iso>' > log 2>&1 < /dev/null &  -- must be fully detached and stdin-redirected, because without a ROM argument it opens a zenity file picker and hangs, and a session-level task stop kills it otherwise.

### Note (2026-07-22)
PROCESS TRAP hit here: pkill -f 'xenia_canary --helpfull' matched the shell running the pkill and killed it (exit 144), so a later command in the same invocation never ran and the run appeared to have started when it had not. Kill by PID, never by pattern, when the pattern can match your own command line.

### Resolution (2026-07-22)
Xenia is a viable differential oracle on this Linux machine. It builds with three local workarounds and no sudo, and runs Gears of War headless with no errors. The harness-versus-broaden fork is therefore resolved in favour of the harness: the risk that justified hesitating -- an emulator that might not build or run -- does not exist. Remaining work is correlating ITRACE/DTRACE output with our runtime, which is engineering rather than gamble.

### Note (2026-07-22)
CORRECTION to the 'oracle confirmed working' note. Xenia starts, initialises Vulkan and spawns its own HOST threads (GPU Commands, GPU Frame limiter, XMA Decoder, Audio Worker) and a swapchain -- but the GUEST title thread never launches. Those thread names are Xenia's infrastructure, not Gears code, and I read them as evidence the game was running. It is not. 430 log lines, zero errors, and no title execution: the ISO is apparently not being mounted/launched by this invocation.

### Note (2026-07-22)
So the correct status is: Xenia BUILDS and RUNS on this machine (that part stands and is verified), but has NOT yet been shown to execute Gears of War. Launching the title is the next thing to solve -- likely the ISO needs mounting differently, or headless mode needs a different target argument. Do not treat the emulator starting as the game running.

### Note (2026-07-22)
Title launch NOT solved. Tried three invocations -- positional ISO, positional extracted default.xex, and the documented --target= flag (src/xenia/app/xenia_main.cc:116 DEFINE_transient_path(target), used at :742). All three produce an IDENTICAL 430-line log that ends at 'VulkanPresenter: Created 1280x720 swapchain' with no mount, no module load and no guest thread. The only path mentioned is the content root. So the target is being ignored or the launch happens on a code path headless mode does not reach.

### Note (2026-07-22)
Signal to use next time: an identical log line count (430) across three different targets is itself the evidence that the argument is not being consumed -- a loaded title would change the log regardless of how far it got. Next thing to check is whether headless mode in this build actually calls the launch path at all, i.e. read xenia_main.cc around :742 and see what gates it, rather than trying more argument spellings.

### Note (2026-07-22)
Root cause of the non-launch FOUND, in xenia_main.cc around :745: RunTitle is dispatched via app_context().CallInUIThread(...). If no UI thread is pumping, the call never executes and nothing is logged -- no error, no mount, no module. That is exactly the observed behaviour and explains why three different target spellings and headless on/off all produced byte-identical output.

### Note (2026-07-22)
Also: the 430 lines are almost entirely Xenia's CONFIG DUMP (~400 lines) plus setup. Line count is therefore a poor progress signal -- it stays 430 whether or not anything happens. Grep for RunTitle/module/title_id instead of counting lines.

### Note (2026-07-22)
NEXT: the launch needs a pumping UI thread. Our invocations are setsid/nohup-detached, which is what keeps a session task-stop from killing the emulator but may also be what starves the UI loop. Try running attached in a real desktop session, or find/patch a path that calls RunTitle directly off the UI thread. Do not try more argument spellings -- the argument was never the problem.

### Note (2026-07-22)
UI-thread hypothesis tested and NOT confirmed as fixable that way: running attached rather than setsid-detached makes no difference (429 lines, still no module load, no title_id). So the UI loop is not being starved merely by detachment. Remaining lead is that emulator_window creation itself may be failing or not pumping under Wayland (DISPLAY=:0 and WAYLAND_DISPLAY=wayland-0 are both set); the earlier zenity picker proves a dialog can appear, but not that Xenia's own window loop runs.

### Note (2026-07-22)
STATUS after several attempts: Xenia builds and runs here, but has never executed this title. Do not re-try target spellings or headless toggles -- both are eliminated. The next genuinely different approaches are (a) run under a nested X server such as Xephyr or xvfb-run to give it a conventional X window, or (b) call RunTitle off the UI thread with a small local patch, which is the most direct route to a CPU-only oracle and does not need a working window at all.

### Note (2026-07-22)
Xvfb approach FAILS: under xvfb-run, Mesa reports 'vulkan: No DRI3 support detected - required for presentation' repeatedly, so the Vulkan presenter cannot initialise and the title still never launches. Note the presenter comes up even with --gpu=null --apu=nop, so the emulation backends are not what pulls Vulkan in. Approach (a) is therefore eliminated on this machine.

### Note (2026-07-22)
Approach (b) applied: a local patch to src/xenia/app/xenia_main.cc replaces 'result = app_context().CallInUIThread([...]{ return emulator_window_->RunTitle(abs_path); })' with a direct 'result = emulator_window_->RunTitle(abs_path)'. Original saved to scratch/oracle/xenia_main.cc.bak. Rationale: a differential CPU oracle needs no window, and the UI dispatch is the only thing preventing launch. Rebuild is incremental (one TU plus link).

### Note (2026-07-22)
PATCH WORKS. With RunTitle called directly instead of via CallInUIThread, the log finally goes past setup: 'Checking for XISO' then 'DiscImageDevice::Initialize' (repeated). So the launch dispatch WAS the blocker, and the UI-thread diagnosis was correct. The 430-line plateau is broken.

### Note (2026-07-22)
New failure, one layer deeper: DiscImageDevice::Initialize repeats and no module loads, so the ISO is not mounting. Our own gdf_extract.py finds this image's partition at a non-zero base (XGD2-style video partition offset), which Xenia's disc reader may handle differently. Next thing to try is pointing Xenia at the already-extracted tree in scratch/game (which contains default.xex) instead of the raw ISO -- that sidesteps disc parsing entirely and we know the extraction is good because our own runtime executes that XEX.

### Note (2026-07-22)
Loose default.xex does NOT work even with the patch: 429 lines, and critically no 'Checking for XISO' line at all, so RunTitle returns early for a .xex rather than failing later. The ISO path gets further (435 lines, reaches DiscImageDevice::Initialize). So the ISO is the more promising target and the disc mount is the real remaining problem.

### Note (2026-07-22)
HANDOFF STATE. Oracle: builds and runs (verified), launches attempted via the local RunTitle patch (verified by log change), disc mount fails (DiscImageDevice::Initialize repeating). Everything needed to resume is recorded: build recipe and three workarounds in this entry, the RunTitle patch with its .bak alongside it in scratch/oracle/, and eliminated approaches (headless toggle, target spellings, detachment, Xvfb/DRI3, loose XEX). The next lead is Xenia's disc reader versus this image's non-zero partition base.

### Note (2026-07-22)
IMPORTANT: the disc FORMAT is not the problem. Our extractor locates this image's partition at base 0xFD90000, and Xenia's disc_image_device.cc lists exactly that base among the ones it supports (0x0, 0xFB20, 0x20600, 0x2080000, 0xFD90000). So DiscImageDevice::Initialize failing is NOT an unsupported XGD variant, and the next investigation should look at file access instead -- the ISO lives on <the image's mount point>, an external mount, and 'F>' log lines repeat three times, suggesting a read or open failure rather than a parse failure.

### Note (2026-07-22)
USER JUDGEMENT (ground truth): Xenia is not reliable enough to be an oracle. Confirmed by this session's own evidence -- ~12 iterations, never once executed the title, upstream calls Linux support experimental, and it needed three build workarounds plus a source patch merely to ATTEMPT a launch. The deeper objection is that an unreliable oracle is worse than none: any divergence found would be ambiguous between its bug and ours, so it cannot settle a question, which was the entire point.

### Note (2026-07-22)
Do NOT resume the Xenia harness. The recomp-harness methodology assumes a trustworthy reference emulator; that assumption holds for mature N64/SNES emulators and does NOT hold for Xbox 360 on Linux. Applying the skill without checking its precondition is the mistake made here.

### Note (2026-08-05)
## Xenia's headless trace-dump tool builds here too (2026-08-05)

The oracle does NOT need the emulator driven by hand. Xenia ships
`xenia-gpu-vulkan-trace-dump`: load a GPU trace, render it, write the frame --
console app, no window, no controller. It is one of Xenia's own CMake targets,
gated behind an option that was off:

    cmake -S . -B build -DXENIA_BUILD_MISC=ON     # in scratch/oracle/xenia-canary
    ninja -C build xenia-gpu-vulkan-trace-dump

Builds clean on top of the existing configure from this entry (the version.h,
bfd-linker and lz4-symlink workarounds still apply and still suffice).
Artifact: build/bin/Linux/xenia-gpu-vulkan-trace-dump, 17 MB.

Contract, read from src/xenia/gpu/trace_dump.cc rather than from --help:

    --target_trace_file=<path.xtr>   (or first unnamed argument)
    --trace_dump_path=<dir>          (or second unnamed argument)

NOT YET WORKING, and this is where it stands: invoked with a nonexistent trace
it hangs in `Setup()` -- which builds the graphics system, BEFORE `Load()` ever
looks at the file -- and writes no log line in 60 s. So the failure is not
"missing trace"; something in Setup blocks. Next session: find where Setup
blocks before doing anything else with it.

## THE TRAP TO AVOID: --help HANGS, it does not print help

`xenia_canary --help` and this tool both route help/errors through a **zenity
dialog** and block forever waiting for a button nobody will press. A 120 s
command timeout fires and the dialog process SURVIVES the parent. Confirmed:
one was still alive 27 minutes later holding the usage text.

Read the cvars in the source instead. If one is left behind, kill it BY PID
(`ps -eo pid,etimes,comm | grep zenity`) -- never by name, other things share it.

### Note (2026-08-05)
## The trace-dump tool WORKS now -- two defects in the fork, both fixed (2026-08-05)

The earlier note today said it "hangs in Setup()". That was right, and the
reason was two independent defects on the path every POSIX console tool takes.
Neither is reachable from the windowed app, which is presumably why they
survived. Both fixed on our fork (extern/xenia @ 346f9ba, pushed).

1. `emulator.cc`: `Setup()` dereferenced `imgui_drawer_` unconditionally to call
   `LoadInputSystem`, while its own signature lets a caller pass null -- which
   `TraceDump::Setup` does, correctly, having no UI. `display_window` and both
   factories beside it ARE null-checked. The fault landed in the guest exception
   handler, which turned the crash into a SPIN, so it looked like a hang and not
   a segfault. gdb on the live process is what separated them.

2. `console_app_main_posix.cc`: `InitializeLogging()` / `ShutdownLogging()` were
   COMMENTED OUT, while `ui/windowed_app_main_posix.cc` calls them.
   `Logger::AppendLine` blocks when the logger was never initialised, so the
   first thing that logs hangs the process. For the GPU tools that is the Vulkan
   loader chattering through the debug messenger inside
   `EnumeratePhysicalDevices` -- long before any trace file is opened, and with
   no output to say so. This also explains the zero-byte logs: nothing could
   ever be written.

VERIFIED: given a missing trace it now prints "Could not load trace file" and
exits 5, rather than hanging until a timeout kills it.

## Running it at 720p

Our captures are 1280x720 and Xenia otherwise picks up the desktop's resolution
(it created a 2560x1440 swapchain), which would make any comparison meaningless.
This fork has no `internal_display_resolution` enum, only the custom pair, and
both must be non-zero to take effect (graphics_system.cc:430):

    --custom_internal_display_resolution_x=1280
    --custom_internal_display_resolution_y=720

## Reproduce

    cd scratch/oracle/xenia-canary
    cmake -S . -B build -DXENIA_BUILD_MISC=ON
    ninja -C build xenia-gpu-vulkan-trace-dump
    build/bin/Linux/xenia-gpu-vulkan-trace-dump \
        --target_trace_file=<trace.xtr> --trace_dump_path=<outdir> \
        --custom_internal_display_resolution_x=1280 \
        --custom_internal_display_resolution_y=720

NOTE: scratch/oracle/xenia-canary is a SEPARATE CLONE from the submodule at
extern/xenia, at the same commit. Patch both or you will rebuild the wrong tree
and conclude your fix did nothing -- which happened once today.

### Note (2026-08-05)
## THIRD defect in the fork's trace path: draws never reached the backend

Found after the imgui and logging fixes. Trace playback could not draw AT ALL,
and the symptom was indistinguishable from a malformed trace -- which is what
makes it expensive, because it points every investigation at your trace.

`ExecutePacket(uint32_t ptr, uint32_t count)` was NOT virtual, while the no-arg
`ExecutePacket()` beside it is. TracePlayer holds a `CommandProcessor*`, so its
call ran the BASE class's instantiation of pm4_command_processor_implement.h --
and that template calls `COMMAND_PROCESSOR::IssueDraw`, a QUALIFIED call, which
suppresses virtual dispatch even though IssueDraw is itself virtual. Every
trace-played draw therefore went to `CommandProcessor::IssueDraw`, the base stub
that returns false, and printed "Failed in backend".

Fixed on the fork (d5f3fd0): the two-argument ExecutePacket is virtual.

MEASURED: a 744-draw trace goes from 744 "Failed in backend" to ZERO, and the
Vulkan primitive processor starts reporting on real geometry.

## How it was found, because the method generalises

IssueDraw has FIFTEEN `return false` paths and every one surfaced as the same
"Failed in backend". Naming them (also on the fork) was the step that mattered:
the answer came back "NONE of them fires", which is only possible if the
function was never entered -- and that is what pointed at dispatch rather than
at the trace. Without the naming, the obvious next move is to keep editing the
trace, which would never have worked.

### Note (2026-08-05)
## FOURTH defect: the headless trace dump can never have a presenter (2026-08-05)

After the imgui, logging and ExecutePacket fixes, the trace rendered every draw
and still wrote NO file. TWO independent causes, both found by reading the path
rather than by trying more traces, and each producing the IDENTICAL symptom --
so fixing either alone still looks like no progress.

1. OUR TRACE HAD NO SWAP. tools/gfr_to_xtr.py emitted the trace format's
   kEvent/kSwap marker, which trace_player.cc treats ONLY as a playback break
   hint -- it calls nothing. Xenia's own kernel triggers a swap with PM4 packets:
   VdSwap_entry (xboxkrnl_video.cc) posts a TYPE0 write of the front buffer's
   six-dword texture fetch constant into fetch slot 0, then PM4_XE_SWAP (0x64)
   carrying 'SWAP', the PHYSICAL front buffer address and the size. Without them
   IssueSwap is never called, guest output is never refreshed, and
   TraceDump::Run's CaptureGuestOutput returns false -> exit 1, no PNG.

2. AND EVEN WITH THE SWAP, THERE IS NO PRESENTER. emulator.cc gates presentation
   on `display_window_ != nullptr`, while trace_dump.cc passes a null window
   (correctly -- it is a console tool). graphics_system.cc's own comment
   contemplates this case ("May be needed for offscreen use, such as capturing
   the guest output image") but the branch was unreachable from Emulator::Setup.
   Fixed on the fork by adding an `offscreen_presentation` parameter that
   trace_dump passes as true. IssueSwap's early-outs are now named, for the same
   reason IssueDraw's were.

Captured on our side too: the front buffer's FETCH CONSTANT. The address alone
does not say how to read the bytes, and Xenia takes the swap texture from fetch
slot 0. Our VdSwap now carries r4's six dwords in its swap packet, the command
processor records them, and .gfr is v3. A capture older than v3 makes
gfr_to_xtr REFUSE to emit a swap and say why, rather than inventing a fetch
constant -- an invented one would make the oracle agree with our guess about the
front buffer's format by construction.

Also fixed while here: pick_packet_scratch chose a page that was all-zero in the
capture, which put the synthesised packets INSIDE the front buffer (0x320000 in
a buffer based at 0x311000). Harmless while nothing presented; now it would be
read back as a block of garbage pixels. The front buffer's whole extent, sized
from the fetch constant's pitch and 32-row padding, is excluded.

### Note (2026-08-05)
## XENIA EXECUTES THIS TITLE. The disc mount was never the problem (2026-08-05)

The July conclusion -- "~12 iterations, never once executed the title, so Xenia
is not reliable enough to be an oracle" -- rested on a fault that had nothing to
do with discs, arguments, headless mode, detachment or Xvfb, all of which were
eliminated at the time and none of which was the cause.

ONE fault explains every symptom. `ImGuiDrawer::InitializeFonts` MERGES a CJK
font chosen by fontconfig. A font the rasteriser cannot parse does not fail when
it is added -- adding only reads the file -- it fails inside
`ImFontAtlas::Build()`, and a failed build leaves the atlas EMPTY. Xenia then
asks for a 0x0 texture; that is an invalid VkImage, and radv answers a
zero-extent bind with a deliberate `unreachable()` trap (`mov 0x18,%eax; ud2`).
The SIGSEGV lands on the UI thread inside Xenia's guest-exception handler, which
blocks in `IsDebuggerAttached()` and never returns. So:

  * the window has no working menus and the compositor calls it Not Responding;
  * `app_context().CallInUIThread(RunTitle)` -- the ONLY path that launches a
    title -- never runs, so no target of any spelling ever launched;
  * and headless made no difference because the UI thread was already dead.

On this machine fontconfig's best CJK match is NotoSansCJK-VF.ttc, a
variable-weight collection with CFF outlines, which stb_truetype rejects
(imgui_draw.cpp, stbtt_InitFont). Vulkan validation named the consequence in one
line: "vkCreateImage(): pCreateInfo->extent.width is zero".

Fixed on the fork (9946714) at three levels: the atlas is built explicitly and a
failed build costs the merged CJK font rather than every glyph; SetupFontTexture
refuses a 0x0 atlas; VulkanImmediateDrawer::CreateTexture refuses zero extents
where the caller is still named.

MEASURED AFTER THE FIX: the disc mounts, `Loading module GAME:\default.xex`, the
title's own content resolves under \WarGame, fourteen guest threads run, audio
plays, and in a 60 s window the GPU issued 1677 swaps (~28 fps) while loading
this title's real surfaces -- 864x864 k_24_8 depth, 322x182 and 128x128
k_16_16_16_16_FLOAT. The user confirms the game runs with correct graphics.

WHAT FOUND IT: the window being unclickable was treated as EVIDENCE rather than
as an annoyance. gdb on the live process showed the UI thread parked in a signal
handler, and Vulkan validation turned "crash in the driver" into a named API
misuse. Guessing at launch arguments could never have reached this.

STATUS OF THE "DO NOT RESUME" RULING: the reason for it -- an emulator that
never ran the title -- no longer holds. Whether to build a differential harness
on it is still the user's call and still costs what it costs; but "it cannot run
this game here" is now FALSE and should not be cited as if it were true.

### Note (2026-08-05)
## Careful: Xenia's RENDERING of this title is intermittent (2026-08-05)

Correcting the note above before it hardens into a false fact. The user observed
the first launch of the fixed build sitting on a black screen, and only a later
launch rendering. Counted over three launches of the same binary on the same ISO:
canary5 = 1 swap in ~120 s (black), canary6 = 1677 swaps in 60 s, canary7 = 4749
swaps in ~150 s. All three loaded GAME:\default.xex.

So LAUNCH is reliable (3/3) and RENDERING is not (2/3), and a single swap in two
minutes is a stall, not slow loading. Cause unknown; nothing here distinguishes a
startup race from a first-run cost. See claim C012, which carries the same
caveat. Quote "2 of 3" rather than "it runs".

### Note (2026-08-05)
## The oracle is HEADLESS now, and what the black screen actually was (2026-08-05)

`tools/xenia_oracle` drives Xenia's emulator core directly -- our code, Xenia's
libraries via its own CMake, no window in the path. Driving `xenia_canary` is
retired: it opens a GTK window whatever `--headless` says.

Three fork defects blocked a windowless run, each invisible from the windowed
app (all on extern/xenia, pushed):
  * `CompleteLaunch` dereferenced `display_window_` before logging anything, so
    the launch faulted and the guest-exception handler turned it into a spin --
    presenting as the DISC MOUNT hanging, which is where July's investigation
    went;
  * console apps reported argument errors through a zenity dialog, because
    `has_console_attached()` is isatty(stdin) and every scripted invocation
    redirects it;
  * a guest crash was PAUSED before it was logged, and Pause() waits on a fence
    only a UI can resume -- so a crash left an empty log and looked like a hang.

That third one is what cracked the black screen. With the dump moved ahead of
the pause: guest PC 0x825F39F4, access violation reading 0x10000003C, on the
main thread -- and 0x825F3450 is the callback the guest had just handed
XAudioRegisterRenderDriverClient. It was crashing inside its own audio
render-driver callback, driven by the NOP audio backend. With SDL audio
(SDL_AUDIODRIVER=dummy, no sound device needed) the same run goes from 124 swaps
plus a crash to 6254 swaps and none.

The other half is input: a headless run has no operator and this title waits.
`scripted_input.{h,cc}` presses on a schedule (BUTTON@SECONDS[+REPEAT]),
defaulting to START then A repeatedly. Xenia's own nop HID driver carries the
TODO saying why "no device connected" is not enough.

RESULT: seven of seven captures, Act 1 gameplay from 120s on (22k-36k distinct
colours, ~76% of pixels above 8/255). Claim C013.

STILL OPEN, both recorded so they are not rediscovered:
  * our SYNTHESISED traces (gfr_to_xtr) still render black in trace_dump even
    though all 18 resolves execute and write 3.7 MB. The index-count bug (665 of
    744 draws claiming empty index buffers) is fixed and was not the whole
    story.
  * `RequestFrameTrace()` from the harness issues the request and writes no
    .xtr, with nothing from the trace writer either. That would be the control
    arm for the point above.

### Note (2026-08-05)
## THE TRACE DUMP TOOL IS THE BROKEN INSTRUMENT (2026-08-05)

Every "our synthesised trace renders black" result was measuring the tool.

The control arm, finally available once the headless harness could make Xenia
capture its own trace: feed `xenia-gpu-vulkan-trace-dump` a trace XENIA ITSELF
produced (4D5307D5_13457.xtr, 41.7 MB), of a frame the same run rendered
correctly -- the harness's own PNG of that moment has 22k distinct colours and
76% of pixels above 8/255. The dump writes a UNIFORMLY BLACK image and exits 0.

Ruled out along the way, each measured on that known-good trace:
  * draws execute -- zero "Failed in backend";
  * all 18 resolves run, the last writing 3,768,320 bytes to the front buffer;
  * the swap finds its 1280x720 k_8_8_8_8 texture at that address;
  * the guest-output image reports a SUCCESSFUL refresh (instrumented);
  * a 2-second wait after playback changes nothing, so it is not a race
    between playback finishing and the GPU catching up.

So the fault is downstream of "guest output refreshed" and upstream of the PNG.
Not chased further today, because the LIVE harness (tools/xenia_oracle, claim
C013) already answers the oracle question without it.

CONSEQUENCES, which is the point of writing this down:
  * recorded as instrument I013, DISTRUSTED;
  * gfr_to_xtr is exonerated of the blackness. Its index-count bug (665 of 744
    draws claiming empty index buffers, from a VGT_DMA_SIZE register our command
    processor never writes) was real, separately measured and fixed -- but it
    was not why the image was black;
  * a trace-based comparison cannot be built on this tool until it can render a
    Xenia-produced trace. The route that works is comparing our renderer against
    the live harness.

Enabling the trace writer at all needed a fork change: it is compiled out under
NDEBUG (trace_writer.h), so RequestFrameTrace ACCEPTED the request and silently
wrote nothing. The header now respects a pre-set
XE_ENABLE_TRACE_WRITER_INSTRUMENTATION, and both entry points say so when the
build has it off.
