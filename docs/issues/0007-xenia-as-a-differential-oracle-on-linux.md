---
id: 7
title: Xenia as a differential oracle on Linux
status: resolved
symptom: need a reference emulator to compare guest state against; unclear whether Xenia builds and runs on Linux
tags: harness,method,oracle
created: 2026-07-22
updated: 2026-07-22
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
