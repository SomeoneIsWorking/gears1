# Project state

Comparison baseline: retail Xbox 360 Gears titles running in an emulator.

This inventory reports observable capabilities independently of the product goals.
`verified` means exercised by the cited test or recorded real-title evidence;
`partial` names the remaining gap; `missing` means no product implementation exists.

| ID | Capability | State | Dependencies | Goals |
|---|---|---|---|---|
| S001 | Exact Gears 1 executable identity and normalized image validation | verified | — | G002, G004 |
| S002 | Bounded user-image/archive provisioning without derived guest source | partial | S001 | G001, G004 |
| S003 | Executor-independent native renderer and RHI contracts | partial | — | G001, G002, G003 |
| S004 | Native Gears 1 audio-mix operation | verified | S006 | G001, G002 |
| S005 | Native notified operation-kind-3 GPU ticket wait | partial | S006 | G001, G002 |
| S006 | Xenia-backed `x360port` execution boundary | partial | S001 | G001, G002 |
| S007 | Gears 1 leaf/import/override discriminator | partial | S001, S006 | G001, G002 |
| S008 | Bounded runtime interpreter fallback | partial | S006 | G001, G002 |
| S009 | Representative interactive Gears 1 gameplay | partial | S002, S003, S004, S005, S006, S007, S008 | G001, G002 |
| S010 | Apple Silicon macOS A64 execution | missing | S006 | G001, G004 |
| S011 | Android arm64-v8a A64 execution | missing | S006 | G001, G004 |
| S012 | Complete native RHI frontend | missing | S003, S006 | G001, G002, G003 |
| S013 | Native 8.33 ms / 120 fps renderer budget | partial | S009, S012 | G003 |
| S014 | Gears 2, Gears 3, and Judgment exact-revision conformance | missing | S009, S013 | G001, G002 |
| S015 | Generated guest-source product and translator-only surfaces absent | verified | — | G001, G002, G004 |
| S016 | Independently authored shared UE3/Xbox contract | verified | S006 | G001 |
| S017 | Asset-free native/JIT boundary CI | partial | S006 | G001, G002, G004 |
| S018 | PC keyboard and mouse controls beside host gamepads | partial | S009 | G001 |
| S019 | Campaign checkpoints save and resume in the player's user-data directory | verified | S006 | G001 |

## Current focus

S009 is the current focus. Gears 1 is the only active title. `./run.sh` now authenticates
the user's disc by its `default.xex` digest, builds the `gears1` product, and runs it
through `x360port::SystemSession`: Xenia's kernel, file system, GPU, audio, and input
services host the authenticated executable, the guest CPU runs on Xenia's x64 dynarec,
and title-owned native overrides are dispatched from the guest's call slots. A headless
150 s run under the profile's menu walk passes the logos and every front-end menu, loads
Act 1, and plays its opening scene at 118-119 presents/s.

The service-binding frontier recorded by `docs/issues/0171` is superseded for the
product: the system session answers every import with Xenia's kernel instead of a
title-local claim table. The profile's gameplay walk now plays past the opening scene to Act 1's first
path choice, and play driven over the control channel reaches Act 1's first firefight.
`tools/combat_route.py` plays from there into the firefight reproducibly by steering
on the local player's position read over the control channel. The one native owner in
play, the audio mix (about 47,000 calls per second, `docs/issues/0172`), agrees with the
guest's own body on every live call of that route (S004), and the route fails unless
the world's game clock keeps to wall time while the product presents up to 120 times a
second, twice the console's rate (0.9997 game seconds per wall second over the route).
Its first idle view renders as stock Xenia renders it (`tools/oracle_compare.py`). The route
now clears that firefight and joins Dom along the level's navigation graph; what
remains for S009 is playing on through Act 1 unattended (a REVIVE tutorial and further
firefights follow) and loading later levels. Presentation now runs
up to 120 presents/s under a host limit; S013 records the gameplay rate and its next costs.

Two facts from an earlier qualification constrain further native-override work. Recovered guest
addresses are addresses in the image as the XEX loader leaves it, with each section at its
raw offset; a copy re-laid by section VirtualAddress displaces everything from `.text` on
(0x4E00 at `.text`), and `tools/guest_image.py` refuses it. And a guest fault inside a translated block strands the calling
thread: the translated-block budget is checked at block entry, so it cannot interrupt an
instruction that faults repeatedly. Entering a wrong address is therefore a hang, not a
typed failure.

## Capability details

### S001 — exact title identity

Evidence: `config/titles/gears1.toml`, `tools/title_identity.py`, and the
title-identity tests fail closed on container and normalized-image hashes.

### S002 — bounded provisioning

Evidence: GDF extraction, archive bounds, and identity are tested. `./run.sh`
resolves the disc from `--iso`, `GEARS_ISO`/`.env`, or one `roms/` drop-in,
materializes a 7z archive under `scratch/titles/archives/`, and refuses every disc
whose `default.xex` SHA-256, read in place from its GDF volume, is not the profile's
`xex_sha256` (`tests/test_bootstrap.py` covers the matching, wrong, missing-executable,
and unreadable-disc cases). It refuses missing build tools and pkg-config modules by
name with the exact Fedora or Debian install command. Gap: there is no no-terminal
first-run setup screen, and no packaged (AppImage/APK) delivery.

### S003 — executor-independent native rendering

Evidence: focused native frame, draw, resolve, resource, and shader tests build
without a CPU executor. Gap: no live Xenia-fed native frame exists.

### S004 — native audio mix

Evidence: `runtime/titles/gears1/audio_mix.*` owns the independently authored
kernel at guest address `0x825F2D40`. The runtime owner installs the override
only when the authenticated image contains that exact address, and the handler
uses x360port's validated mapped-memory contract: it stages its input and output blocks
with one validated read each and writes the output back once.
`tests/test_gears1_real_leaf.cpp` qualifies it differentially on the real image:
the native override and `CallOriginal` on the original guest body agree on the
return value and on every byte of the arena with disjoint, in-place and overlapping
input and output blocks. Faithfulness required reproducing the
guest's r3 result and `vmaddfp`'s fused multiply-add with denormal-input
flushing. Claim C099 records the falsifier.
The same comparison runs on live play: `--verify-audio-mix` (through
`tools/run_offscreen.py` or `tools/combat_route.py`) installs
`audio_mix_differential.*`, which on every call snapshots both blocks, runs the native
mix, restores the snapshot, runs the original body through Xenia, compares both blocks
and r3, and keeps the original's result. Its unit test proves it locates output, input
and r3 disagreements and undoes the native writes. Over the whole 720 s combat route,
into Act 1's first firefight, it compared 33,406,491 calls with 0 disagreements and
0 calls left uncompared.

### S005 — native GPU ticket wait

Evidence: `runtime/gpu_ticket_wait.*` and `runtime/wait_probe.*` retain the host
synchronization contract. Gap: guest-address dispatch through `x360port` is
missing.

### S006 — Xenia-backed execution

Evidence: `test_gears1_dynarec_boundary` consumes exact shared-runtime and Xenia
revisions, maps an aligned authenticated synthetic image whose code and entry
point use the retained Gears leaf address, and crosses JIT to typed import to
native code. The pinned shared runtime also now exposes and tests image-scoped
native override, scoped original-call, exact-entry invalidation, and
title-neutral device-memory callback contracts. It validates title-reported
executable-write ranges and now automatically observes virtual writes to the
authenticated code range, exits an active translated call after a watched guest
store, invalidates touched cached functions before the next guest entry, and
preserves unrelated translations. The shared runtime also bounds translated
basic-block execution across nested guest calls and returns typed exhaustion and
invalidation results. A host import can refuse explicitly; the translated call
exits with its library, ordinal, and reason while incrementing a refusal count,
rather than continuing with an invented return value. The pinned Xenia fork now
routes compiled direct and indirect guest calls through invalidatable entries;
the shared synthetic regression modifies a leaf and observes its new result
through a previously translated caller. The updated shared contract also routes
a cached guest caller to a native override, preserves scoped original calls
through callee invalidation, propagates typed callback failure, and restores the
original call after override removal. The headless real Gears-image AddRef
discriminator now exercises the same native-override route from a nested guest
call and verifies restoration after removal. The pinned shared runtime now
refuses an invalid PPC opcode before host-code publication and routes
decoded-but-unimplemented fixtures through a bounded Xenia-owned interpreter. The fallback covers
`lswi`, integer immediates, big-endian scalar loads/stores, comparisons, and conditional branches,
and records entries, executed instructions, unsupported instructions, memory failures, and budget
exhaustion; the valid cached PPC function still executes through the JIT.

`runtime/gears1_guest_image.*` now consumes
the shared PE layout owner and seals a flat guest module after exact normalized-image
profile authentication. `runtime/gears1_runtime.*` composes that sealed module with a
single Xenia runtime context, persistent variable-import storage, the title-owned
`XGetAVPack` service, and fail-closed unknown function imports; its synthetic entry test
executes the authenticated adapter through Xenia's JIT and returns the configured AV pack.
The shared checked-XEX2 inspector also validates the real ignored container, normalized
image, import manifest, and helper-pattern evidence. The product now hosts the
authenticated disc on `x360port::SystemSession` (S009). Gap: complete fallback
ISA/control-flow coverage and title-specific write/cache-control semantics remain
missing.

### S007 — first Gears discriminator

Evidence: the synthetic Gears-addressed module executes through Xenia's JIT and
calls a typed `DbgPrint` binding; the pinned shared runtime separately proves
cached guest-to-native override/original-call routing against a synthetic guest leaf.
The runtime-composition test authenticates the synthetic profile image through the Gears
adapter, binds title-owned `XGetAVPack` ordinal 971, and returns its configured value through
Xenia's translated entry path.
The maintained headless discriminator now initializes `Gears1Runtime` from the
profile-matching ignored image; that owner maps its PE sections, allocates and
initializes the caller-owned guest object and variable-import storage, and
invokes a retained real-image function-import thunk through its composed manifest,
and executes real leaf `0x8222E868`: it returns `0x5`, the native override
calls the real original once, and reported executable-write invalidation restores
the original path. A second real-image fixture activates the leaf's own nested
guest call at `0x8222E8C0`: outer and inner calls both enter the native override,
then removal restores the nested original call and its inner refcount effect.
The production `Gears1GuestImage` adapter now authenticates
the same normalized image and import manifest; its synthetic CTest and real
ignored-input discriminator pass. The shared checked-XEX2 inspector provides the
container, normalized image, 236 logical imports under the correctly indexed
`xam.xex` and `xboxkrnl.exe` entries, and eight helper-pattern hits.
The first real-image function-import thunk reaches its title callback and now
returns `ImportServiceRefused` for an unsupported service; all
real-image variable imports resolve into bounded caller-owned guest storage.
The title-owned `XGetAVPack` binding executes through its real thunk and returns
the configured AV-pack value. Every host service now claims the exports it
implements by name through the pinned shared claim table, which resolves those
names against Xenia's ordinal tables and refuses a name the library does not
declare, an export two services both claim, a handler on a variable export, and
a claim with no handler. No ordinal literal remains in this title's bindings or
in the real-image discriminator's manifest assertions. The real
`XamInputGetState` ordinal 401 thunk consumes the retained Gears input owner's
coherent remote-pad snapshot through shared `x360port` XAM serialization. The
headless ignored-XEX discriminator verifies all 16 guest bytes, a connected
null-pointer query, the one-local-user policy, disconnection clearing stale
state, and typed refusal of an unmapped state pointer. The asset-free remote
input test preserves packet and source-arbitration behavior. The real
`XamInputGetCapabilities` ordinal 400 thunk now reports the title's virtual-pad
capability policy as a checked 20-byte guest record; the headless discriminator
also proves disconnected zeroing, null-pointer bad arguments, and typed refusal
of unmapped memory. The superseded direct-memory handlers were removed. Gap:
vibration output and the remaining title-specific service/device semantics, plus
the complete product launch path, remain unimplemented.

### S008 — bounded interpreter fallback

Partial capability: after Xenia compilation refusal, `x360port` now selects the Xenia-owned
bounded interpreter and reports typed unsupported-instruction, memory, and instruction-budget
refusals. The synthetic runtime discriminator proves an invalid opcode is refused without host
code, and proves `lswi`, integer immediates, big-endian scalar loads/stores, comparisons, and
conditional branches execute with counted fallback entries/instructions. Explicit interpreter mode
remains diagnostic-only and fallback is not gameplay or performance evidence. Gap: the interpreter
currently covers only this narrow subset; complete PPC ISA semantics, safe guest control flow,
imports/devices, real-image fallback, and title gameplay remain open.

### S009 — representative gameplay

Evidence: `tools/run_offscreen.py --seconds 150 --walk menu` runs the shipping
executable headless and silent on the supported disc. It presents at 30/s while the
title boots and 120/s from the front end on (the host cap, S013). Its captures show the logos; the main, campaign,
single-player, and difficulty menus; the unsigned-profile prompt; and Act 1's opening
scene with its subtitles. The run fails unless the dynarec translated guest code and
no function failed to translate: over 60 s it translated 10,996 guest functions to
24.8 MB of host code with 0 failures. The system session has no interpreter fallback.
`--walk gameplay` continues past that scene under scripted stick, trigger and button
input: Marcus walks out of the cell block to the "Choose path: Combat / Training" prompt
with Dom ahead of him, takes the combat path with LT, and walks on while Dom radios
Delta. The run has 8989 presents in 305 s and 13,988 translated
functions with 0 failures.
A control override on the draw entry counted 211 guest
calls in 8 s, so dispatch and original-body calls work on real guest threads.
Driven on from there through the offscreen control channel (`docs/interactive-debug.md`),
the product plays into Act 1's first firefight: Marcus satisfies the look (hold Y) and
objectives (hold LB) tutorials, which each hold the player still until their button is
held; homes on the "Door to prison blocks" by walking with Y held; kicks the jammed door
open with X; crosses the dark cell room to the yard; takes cover with A; revives Dom, who
is downed by Locust fire; and aims with LT and fires with RT, the ammunition count falling
from 312 to 298. A fixed-time script does not repeat this route: the tutorial holds begin
where the player happens to be, and two runs of the same walk ended in different places.
`tools/combat_route.py` repeats it closed-loop: it starts a fresh `--walk gameplay` run,
reads the local player (`GET /api/player`, chain in `docs/re-frontier.md`, "Local player"),
steers the stick toward each measured position, answers a tutorial prompt only when the
player stalls, and passes only when the player reaches the yard cover and the weapon's
magazine count rises. Two consecutive fresh runs passed with the same trace: the door
opened 30.8 s after arrival on the third kick attempt, the yard legs took 1.4-1.8 s
each, and the weapon fired 6 and 5 rounds on the second cover attempt. The native audio
mix is compared with the guest's body on every call of that route (S004). The route also
reads the world's game clock (WorldInfo `+0x288`) and fails unless it advanced within
3% of wall time; the last run measured 0.9997 at 119-120 presents/s, so the doubled
presentation rate does not speed up the simulation. `tools/oracle_compare.py` runs
stock Xenia (`tools/xenia_oracle`, signed in like the product) and then the product on
the menu walk for 240 s each, so both stand idle in Act 1's cell block. It passes when
the product's last capture differs from the oracle's by at most twice the oracle's own
change between its last two captures (`tools/frame_parity.py`). Measured: 0.351
against a limit of 0.509 (oracle motion 0.255). Planted defects in the product frame
score 1.04-7.9 against an oracle motion of 0.32: 10% brightness, a 120x200 hole, a
2-pixel shift, gamma 0.9, swapped red and blue, black. The product draws through the
same Xenia GPU backend as the oracle, so what the comparison can catch is the product's
own composition (the 1000 Hz vblank and the present cap, S013); its one native override
in play is checked on every call instead (S004). The firefight's frames are not
compared, because the oracle has no control channel to follow the closed-loop route.
The route now fights: it reads every pawn's health and team (`/api/player`), aims from
the camera at each living hostile, fires half-second bursts, recovers in cover below 280
health, switches to the pistol when the Lancer runs dry, and after a death reloads the
yard checkpoint and resumes, up to four attempts. When the drones are dead it waits for
Dom to stop and joins him along the shortest walk-and-mantle path of the level's
navigation graph (`/api/navigation`, `tools/navigation.py`), then ends the run through
`POST /api/stop`. A fresh run on 2026-09-24 passed end to end: all four drones killed
on the first attempt in 66 s with no death, Dom joined over 13 graph points and one
mantle, game time 0.9996 of wall time over 145 s, and the run stopped after 459 s with
p50 8.4 ms and 0 translation failures. Two runs before it failed at the jammed door
(the kicks missed while the walk pressed Marcus against it; the route now steps back
first) and at a navigation path with a null end (now reported rather than refused).
Driven by hand from there with the same steps, Marcus followed Dom, cleared the second
firefight (three drones, 51 s, full health) and reached `WarCheckpoint_3` of
`SP_Prison_S05_Scripting`, where a REVIVE tutorial for a downed Dom stopped the walk.
Gap: the route does not yet play past joining Dom: the second firefight, the revive
(A to dismiss the tutorial, X beside Dom), and everything after `WarCheckpoint_3` are
unscripted, and no later act or level load has been exercised.

### S010 — Apple Silicon A64

Missing capability: qualify A64 code emission, executable memory,
instruction-cache coherence, host ABI, exceptions, and packaging on Apple
Silicon macOS.

The macOS CI job fails at link with 63 undefined `xe::` symbols, and the cause is
measured rather than assumed: `xe_platform_sources` in the fork's
`cmake/XeniaHelpers.cmake` excludes every platform suffix and then restores them
only under `WIN32` and `CMAKE_SYSTEM_NAME STREQUAL "Linux"`. On Apple no platform
source is compiled at all — not the `_posix` files and not the two `_mac` ones.

An Apple branch is not a one-line addition. `threading_posix.cc` is already
partly Darwin-aware (its `NanoSleepPrecise` has a `mach_wait_until` path and its
`ticks()` is commented out beside a mach note) while still calling
`syscall(SYS_gettid)`, which Darwin does not provide; and `threading_mac.cc` and
`debugging_mac.cc` redefine functions their `_posix` counterparts also define.
Compiling both sets duplicates symbols and superseding by file stem would drop
the thread, event, semaphore, and timer implementations that exist only in the
posix file. The work is per-function guards in the fork, not a CMake branch
alone.

### S011 — Android A64

Missing capability: qualify A64 code emission, executable memory,
instruction-cache coherence, host ABI, lifecycle, and APK packaging on Android
`arm64-v8a`.

### S012 — complete native RHI

Missing capability: bypass guest command construction through the native RHI
and prove frame parity. Existing native pieces do not establish that result.

### S013 — native renderer budget

The console presents Gears 1 every second vblank, and the title's game clock is the
host clock (`runtime/titles/gears1/presentation.h` records the evidence). The title
waits for a vblank at every present, so vblank pacing rounds each frame up to the vblank
grid: at a 240 Hz vblank the GPU command thread waited in `WAIT_REG_MEM` about 3 ms of
every junction frame, holding 12 ms frames to 16.7 ms. The product therefore runs the
vblank at 1000 Hz and caps presents at 120/s on the host (`max_presents_per_second`),
which holds a present only when it arrives early. Measured headless on an AMD Radeon RX
6700 XT (RADV) with the profile's gameplay walk, 300 s (2026-09-23), from the offscreen
run's per-10 s frame-time percentiles (host time between guest presents, 0.1 ms buckets):
the menus, Act 1's opening scene and the cell block hold 120 presents/s at p50 8.4 ms,
p99 8.6-9.3 ms; the first corridor fell to 76-107 presents/s (p50 12.8 ms) in seconds
241-260 of the latest run, while the host's load average rose from 3.7 to 9, where it
had held 120 before; the last 30 s, at the "Choose path" junction, run at 81-114
presents/s, p50 8.6-9.6 ms (240 Hz vblank pacing: about 59, p50 16.8 ms).

Play was bound by Xenia's GPU command thread, not the host GPU (40-80% busy). Three
costs on that thread were removed in the pinned fork: a `gettid` syscall on every
global-mutex acquire (34 to 49 presents/s), a scan of every live occlusion report on
each BEGIN (49 to 63), and oracle shader hashing on each unarmed draw; PM4 register ranges
are now written in bulk rather than through a virtual call per register, and swap and
resolve diagnostics no longer log every frame. The guest render thread spins on GPU
progress (`sub_8222F460`, S005), but slowing that spin cut the thread's CPU without
changing presents per second (issue 0152), so it costs power, not frame rate. The GPU command
thread is busy about 0.8 of a core with a flat per-draw profile. Descriptor sets are no
longer rewritten for every draw: a stage's texture set is kept while its bindings are
unchanged, and the constants are dynamic uniform buffers rebound with new offsets.
Frame times are too noisy on this shared host to show that (junction p50 spread
10.6-12.8 ms for identical code), so it was measured as the command thread's retired
user instructions per present over 15 s at the junction: 99.2M and 99.2M per present
before, 96.8M and 90.7M with texture set reuse, then 97.2M and 96.9M against 94.5M and
94.8M for the dynamic constants. The driver's recording of the deferred command buffer,
a fifth of the thread's time when it ran at the end of each submission, now runs on a
recording thread as the commands fill 32 KiB: 94.7M instructions per present to 77.3M,
junction p50 8.5-9.5 ms against 9.7-14.4 ms for the previous build in alternating runs.
Samplers are now kept across draws with the same shaders in the same submission while
none of the fetch constants they name is written: 77.8M instructions per present to 67.7M.
Storing constants only when their value changes made no difference (77.2M and 77.5M
against 77.5M and 77.4M) and was not kept. Moving the command thread to a core of its own,
with every other product thread off that core, lowered the junction from 94 to 67
presents/s in the same run. The rest of the profile has no dominant cost: the global mutex
is 7% of the thread's instructions and constant setup 11%. Its `WAIT_REG_MEM` on guest word physical `0x1F99E004` is the per-present
vblank wait above: once per frame, and woken by the vblank thread after the title's
vblank handler runs rather than after Xenia's `wait / 0x100` ms poll interval (junction
p50 12.7 to 11.7 ms). Skipping the global mutex for already-valid vertex ranges did not
change the junction rate beyond the run-to-run spread and was not kept. Remembering
which shader an unchanged `IM_LOAD` range produced, under a shared-memory watch, cut the
pipeline cache's shader hashing from 4% of the thread to nothing, but its own lookup took
1.5%, and alternating runs measured 66.2M and 66.1M instructions per present against
66.2M and 66.1M without it, so it was not kept. The
command processor now reports its ring read pointer every RB_BLKSZ rather than once per
batch, and the native audio mix, whose per-vector guest-memory validation had taken 27% of
all process samples at the junction, now takes 8%. `perf report` cannot name translated
guest code, which Xenia keeps in a shared-memory file; `tools/perf_guest_report.py`
resolves a recording against the map `tools/run_offscreen.py --perf-map` writes.

At the junction (s282-290, 999 Hz samples) the guest render thread is the saturated one:
0.99 of a core against 0.92 for the GPU command thread and 0.73 for the game thread.
About 17% of the render thread is its wait on the GPU (`sub_822306A0`, the wait-condition
check, 8.7%, and the `sub_8222F460` poll, 8.6%); the rest is its own rendering work with
no function above 4%. A cheaper command thread shortens that wait; the rest needs the
render thread's work itself to be native (S012).

The combat route's yard firefight (`tools/combat_route.py`) ran at 103-114 presents/s
in one run and fell to 19-50 in another, while another agent's emulator held 2.3 cores
and the load average was 11.8 on 16 cores; in the slow seconds every product thread
fell to about 0.5 of a core together, so those drops measure the host, not the product.
In the yard, the guest render thread was at 0.95-0.98 of a core, as at the junction.
Disabling Xenia's host/guest stack synchronization did not lower the render thread's
instructions per present at the junction (36.5M against 34.1M, within the variation its
GPU-wait spin adds), so it stays enabled.

Gap: gameplay does not yet hold 120 presents/s at the path junction (p50 8.5-9.5 ms there)
or in the yard firefight (103-114 presents/s), where the guest render thread is saturated.

### S014 — later Gears titles

Missing capability: qualify exact revisions of Gears 2, Gears 3, and Judgment
after Gears 1 compatibility and performance gates complete.

### S015 — generated guest-source product absent

Evidence: the build graph, launcher, tests, tools, and docs contain no generated
guest corpus, function map, offline CPU translator, or selectable old product.
The migration-boundary gate checks prospective first-party paths.

### S016 — shared UE3/Xbox contract

Evidence: the pinned public `shared/x360ue3` revision provides title-supplied
ABI/binding-schema validation plus object/resource/frame lifetime and semantic
RHI ordering contracts. Gears consumes it in `test_gears1_ue3_contract` with
positive and controlled-negative cases. Gap: the contract is not yet composed
with the authenticated real-image runtime and native RHI backend.

### S017 — native/JIT boundary CI

All three hosted jobs failed on 2026-09-22 for three unrelated causes, each
fixed at its cause rather than by relaxing a check: the verifier built a
hand-maintained target list that had drifted from the registered CTest set, so
`gears_remote_input` was reported "Not Run" (CMake now owns the set through
`gears_add_binary_test`); Xenia's `emulator.cc` reached vendored `tabulate`
through a non-SYSTEM include path, so clang 22's
`-Wdeprecated-literal-operator` became an error under `-Werror`; and FFmpeg's
platform ladder had no Apple branch, so macOS inherited `CONFIG_ICONV` and
failed to link `iconv`. A local pass does not establish the hosted matrix:
Windows and macOS remain proven only by CI.

Evidence: the immutable workflow defines the Gears-owned production-boundary
discriminator for Linux x86-64, Windows x86-64, and macOS arm64. It also executes
the exact/clean dependency and bootstrap contracts, and the canonical C++ quality
owner formats maintained source and lints the built first-party discriminator.
The canonical `tools/verify_dynarec_boundary.py --x360port-root ../../shared/x360port
--expected-machine x86_64` gate uses the exact `x360port` and Xenia revisions
pinned by CMake and the workflow. It passed all eight CTests locally with Clang
on the pinned `x360port` revision `7c9652d34237e5a18b4e0963b7f9d87aa4ab7985`
with Xenia `7aa5aec030c56abf551a8e2038096de7fe476235`; the headless
real-image import/leaf discriminator passed separately against the ignored
user-supplied XEX through `Gears1Runtime`, resolving 236 imports and executing the retained real leaf,
scoped original, nested guest-call override/removal, and executable invalidation.
Gears consumes the shared
driver-aware warning owner;
production CMake selection and real Clang/clang-cl probes accept modern C++ and
reject an unused parameter. That ownership change preserved every native compile
command, and the focused boundary and quality CTests passed afterward. Xenia's
standalone portability suite stays disabled in this consumer; its framework owns
that separate gate. This is synthetic native/JIT mechanism evidence, not Gears
gameplay conformance.
Gap: hosted qualification remains pending, including the known Windows tabulate
portability fix awaiting a fork-publication decision. Android remains absent
because no APK/runtime owner exists.

### S018 — PC keyboard and mouse controls

The emulator baseline plays only with a gamepad. The windowed product also plays with
the keyboard and mouse: W/A/S/D move, the pointer aims, left and right click fire and
aim, and the remaining actions sit on Space, E, F, R, Q, Tab, 1-4, Escape, and
Backspace (`runtime/titles/gears1/desktop_controls.h`). The first click captures the
pointer; losing focus releases it and every held key. Host gamepads stay live and are
merged with the keyboard's pad by `x360port`, so neither device disconnects the other.
Evidence: `gears1_desktop_controls` covers the bindings, opposing-key cancellation, the
mouse's dead-zone offset, Y inversion and clamp, and the arbitration that lets a remote
pad take the controller and the window's input resume after it. Gaps: no agent run
exercises the GTK capture, which needs a person at the window; bindings are fixed; the
Windows and macOS hosts do not exist yet.

### S019 — campaign saves

On the console a signed-in profile holds the campaign's checkpoints. The product signs
in one local profile, `Player`, before the game starts: `x360port`'s system session
creates it under the storage root on the first run and signs the same profile in on
every later one. The windowed product's storage root is the OS user-data directory;
offscreen runs use `scratch/offscreen/storage`. Before this the game ran with nobody
signed in and wrote no checkpoint. Evidence (2026-09-23, offscreen): the gameplay walk's
menus show "Profile 1: Player", Act 1 writes `default_checkpoint.sav` (the level's first
checkpoint) under the profile's content, and on the next run Single Player offers
Continue Campaign, which resumes the level from that checkpoint; New Campaign then asks
before overwriting it. `system_config_tests` covers the refused gamertags.

