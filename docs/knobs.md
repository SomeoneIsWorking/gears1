# Runtime knobs

Every `GEARS_*` environment variable the runtime and its tools read, in one
place. **This file is the registry** the global rule asks for: diagnostics may
use environment variables, but they go through ONE tracked list so dead ones
become visible instead of accumulating.

Two rules that are not negotiable:

- **Every knob goes through `lucent::config`**, never a raw `std::getenv`. One
  idiom means one place to look, and `flag()` treats `=0`/`false`/`no`/`off` as
  OFF — presence semantics made `GEARS_DRAW_NOTEX=0` *disable* texturing, which
  is a trap.
- **A control arm is never a fix.** Anything marked *control arm* exists to
  isolate a cause by comparison. If one is ever needed to make output correct,
  the defect is elsewhere.

Add a knob here in the same commit that adds it to the code; delete it here in
the same commit that removes it.

## Build

| Knob | Meaning |
|---|---|
| `-DGEARS_PPC_OPT=-O0` | Optimisation level for the 49k translated guest functions (CMake cache, default `-O2`). `-O0` builds in about the same time and makes the title's own code ~2.5x slower; it exists for bisecting a miscompile, not for speed |

## Selecting what runs

| Knob | Meaning |
|---|---|
| `GEARS_DRAW_FRAME_AT=N` | First guest frame to render. Rendering itself is **not** a knob — every frame from here on is drawn. Loading frames carry 2–3 draws; the Act 1 scene phase holds 700+. Default 0 |
| `GEARS_DRAW_FRAME_COUNT=N` | Render N frames then stop. Default `0` = every frame from `_AT` onward, which is the live path; a positive count is what capture and measurement runs use |
| `GEARS_DRAW_FRAME_REPORT_EVERY=N` | Census + screenshot every N rendered frames (~40 ms each, so a visible hitch by design) |
| `GEARS_DRAW_DIR=<dir>` | Where screenshots, dumps and diagnostics are written |
| `GEARS_DRAW_PS_CONSTS=<ps hash>` | Print the pixel float constants a named shader actually received, as numbers. `psconst 9/3 nz` says three of nine are non-zero and cannot say WHICH — and for a pass ending in a scale, "the scale is 0.9" and "the scale is 0" are a frame and a black screen |
| `GEARS_NATIVE_PASSES=1` | Render the passes in `runtime/native_pass.cpp`'s roster with **our own** shaders instead of the title's translated microcode. Off by default. The log names every pass and says which are implemented and which are only declared, and warns when this changes nothing because no pass has a module. Gate: `tools/verify_native_pass.sh` |
| `GEARS_WATCH_FREE=<guest address>` | Reports the moment that address is released through the pool, with the caller that did it. For a use-after-free whose object address is known from a core file: a raw SIGSEGV leaves no clean exit to dump a table at, so the report happens live |
| `GEARS_INPUT_SCRIPT=<steps>` | Timed pad input, e.g. `25000:START,25300:,150000:LY+`. Buttons and stick deflections (`LX/LY/RX/RY` with `+`/`-`), combined with `&`. Only advances when the guest polls, so a headless run is reproducible |
| `GEARS_SAVE_DIR=<dir>` | Where saves live. Defaults to `$XDG_DATA_HOME/gears1`, then `~/.local/share/gears1` — where a Linux game keeps user data, not next to the executable |
| `GEARS_CMDLINE=<text>` | Hand the title a command line, as a console launcher would (via launch data). It reads `NOMOVIE`, `ONETHREAD`, `NOSOUND`, `BENCHMARK`, `DUMPMOVIE`, `NOINI`, `SECONDS=`, `EXEC=`. Verified: `-nomovie` takes the title's `.bik` opens from four to zero |
| `GEARS_NO_WINDOW=1` | Headless |
| `GEARS_NO_VBLANK=1` | Disable the 60 Hz vblank |
| `GEARS_AUDIO_PUMP=0` | Stop driving the guest's audio callback. **On by default now** — the whole chain works, so a run without it is silent on purpose. Costs ~4% of frame rate under a CPU-bound guest |
| `GEARS_AUDIO_OUT=0/1` | Host playback device. On by default in a windowed run, off in a headless one (measurement should not make noise); `=1` forces it on headless, `=0` off |
| `GEARS_AUDIO_WAV=<path>` | Write every submitted frame verbatim to a WAV (6 channels, 48 kHz, 32-bit float). The header is refreshed once a second so a killed run still leaves a readable file |
| `GEARS_XMA_DUMP=<dir>` | On the first kick of each XMA context, dump its 64-byte context and the raw 2 KB packets behind it, for offline decode with `tools/xma_wrap.py` |
| `GEARS_XMA_TAP=<dir>` | Write every byte the XMA decoder commits to a context's output ring (`ctx<N>.pcm`, big-endian int16), for comparison against a golden decode with `tools/xma_compare.py` |
| `GEARS_CP_STALL_MS=N` | Block the command processor N ms at the first swap — the control arm for "did host work perturb the guest?" |

## Capture and replay

| Knob | Meaning |
|---|---|
| `GEARS_DRAW_FRAME_DUMP=<path>` | Write the reported frame's whole draw stream to a capture file |
| *(no knob)* | Every run now samples one real **presented** frame every 300 presents until it finds one with highlights, and says so in a line: `presented frame checked ... 99th-percentile brightness N/255`. A run whose frames never reach the top of the range warns — that is what an untonemapped linear-light buffer looks like on screen, and it is the only check in the runtime that looks at what a person actually sees |
| `GEARS_PRESENT_UNORM=1` | **Control arm.** Present into an untagged UNORM swapchain, the arrangement before the sRGB tag. The frame's bytes are sRGB-encoded either way; this only changes whether the compositor is TOLD. A display where the tagged version looks wrong would be the reason to use it (`catalog.py show 61`) |
| `GEARS_PRESENT_HIDDEN=1` | Create the **real** window -- real Wayland/X11 surface, real compositor, real swapchain -- and never map it. Exercises the window system that `GEARS_PRESENT_HEADLESS` does not, while putting nothing on the operator's screen. This is how the windowed path is measured (`catalog.py show 61`) |
| `GEARS_PRESENT_HEADLESS=1` | Run the whole present path -- swapchain, blit, `vkQueuePresentKHR` -- against `VK_EXT_headless_surface`, with **no window**. This is what makes the present path measurable in a headless run; with `GEARS_PRESENT_DUMP` it captures what would have reached the screen. Refuses loudly if the loader lacks the extension rather than presenting nothing (`catalog.py show 60`) |
| `GEARS_PRESENT_DUMP=N`, `GEARS_PRESENT_DUMP_AT=N`, `GEARS_PRESENT_DUMP_DIR=<dir>` | Write N frames **as presented** — through the swapchain blit — after frame `_AT`. `./run.sh --present-dump N` is the short form. **The only capture that sees the present path**: every other screenshot here comes from the renderer's readback, which is why an sRGB swapchain washed out the entire window while every capture in the repo looked correct (`catalog.py show 60`). Needs a window |
| `GEARS_REPLAY_MIRROR_MB=N` | *(frame_replay)* Override the guest-memory mirror size |
| `GEARS_REPLAY_DUMP_SHADERS=<dir>` | *(frame_replay)* Write every distinct microcode blob, named by hash, for `tools/xenos_translate --raw` |

## Diagnostics — dumps and censuses

| Knob | Meaning |
|---|---|
| `GEARS_DRAW_DIAG=<path.tsv>` | **The per-draw table.** One row per draw joining what it was, what it did (pipeline statistics) and every piece of state that can zero it, with a `verdict` naming the stage it died at. Start here |
| `GEARS_DRAW_RESOLVE_DUMP=1` | Write every resolve target to a PPM — what the guest's post passes actually sample — and log its true **range and non-zero count**. The PPM alone cannot be trusted for non-colour targets: it clamps to [0,1] at 8 bits, so a signed sub-unit buffer (the motion-blur velocity target) writes as pure black. The log line says so when that is the case (`catalog.py show 66`) |
| `GEARS_DRAW_TEX_DUMP=1` | Write each decoded guest texture; `tools/decode_bc.py` turns one into a PNG |
| `GEARS_DRAW_VDUMP=N` | Dump draw N's first vertices at the shader's own stride. N is the `draw` column of the diag table |
| `GEARS_DRAW_STATS=1` | Per-draw pipeline statistics |
| `GEARS_DRAW_CENSUS=1`, `GEARS_DRAW_FRAME_LIST=1` | Per-run and per-draw censuses |
| `GEARS_DRAW_FRAME_STEP=N` | Checkpoint image every N draws |
| `GEARS_DRAW_VALIDATE=1` | Vulkan validation layers |
| `GEARS_SHADER_CAPTURE=1`, `GEARS_SHADER_CAPTURE_DIR=<dir>` | Also write each bound microcode blob (and a `manifest.csv`) to disk for the offline tools. The runtime keeps the microcode in memory unconditionally — the renderer translates the pair bound at each draw — so this knob controls the **copy on disk**, nothing else |
| `GEARS_CONST_DUMP=1` | One-shot dump of the constant/fetch register files at the hot pair's draw; `GEARS_CONST_DUMP_ANY=1` drops the hot-pair filter. Feeds `tools/system_constants` |
| `GEARS_DRAW_CAPTURE=1` | One-shot decode of the hot pair's DRAW_INDX: packet fields, index buffer, vertex fetch constant and the first vertices, to `scratch/draw-params/hot_draw.txt` (`GEARS_DRAW_CAPTURE_DIR` moves it, `GEARS_DRAW_CAPTURE_ANY=1` drops the hot-pair filter). The RE instrument behind catalog #24 |

## Self-validation — proving an instrument can still say "yes"

An instrument whose normal output is nothing cannot be distinguished, from its
log, from an instrument that is broken. Each of these feeds its detector a case
that MUST produce a positive; a silent run with one of them set means the
detector is dead, and every negative that detector has ever printed is worthless.

| Knob | Feeds |
|---|---|
| `GEARS_ASAN_SELFTEST=1` | One deliberate out-of-bounds heap read. A sanitizer build that does not report it is not watching the process. Only exists in a sanitizer build (`runtime/main.cpp`) |
| `GEARS_STALL_SELFTEST=1` | Two synthetic progress channels: `selftest.stopped` ticks once then stops (the stalled branch must fire at 8 s) and `selftest.silent` never ticks (the never-started branch must fire at 40 s). Both lines must appear (`runtime/wait_probe.cpp`) |

Their offline counterparts are `--selftest` subcommands rather than knobs:
`tools/find_addr_refs.py --selftest` and `tools/atomic_audit.py --selftest`.

## Control arms — never fixes

Each isolates one cause by comparison against the default.

| Knob | Isolates |
|---|---|
| `GEARS_DRAW_ONLY=N`, `GEARS_DRAW_ONLY_BASE=<hex>` | One draw, or one EDRAM surface |
| `GEARS_DRAW_NOTEX=1` | Texture content (white stubs) |
| `GEARS_DRAW_NOBLEND=1` | The blend equation |
| `GEARS_DRAW_NODEPTH=1` | Depth rejection |
| `GEARS_DRAW_NOCULL=1`, `GEARS_DRAW_CULL_INVERT=1` | Culling, and the winding convention |
| `GEARS_DRAW_FIXEDVP=1` | The guest's viewport/scissor |
| `GEARS_DRAW_AB_CENSUS=1` | The per-draw viewport census, run on **alternate frames** so both arms share one run. Separate runs cannot resolve anything below ~10 ms a frame here — see `runtime/frame_ab.h` |
| `GEARS_DRAW_DEPTH_CLEAR=<float>` | The depth clear value (the guest's own is used by default) |
| `GEARS_DRAW_SLATE_CLEAR=1` | Restores the diagnostic slate clear. **It is not the guest's colour** — it lifted the HDR surface off zero and hazed the whole frame (catalog #37) |
| `GEARS_DRAW_DEPTHONLY_PS=1` | Runs the pixel shader on depth-only draws, against the hardware contract |
| `GEARS_DRAW_NORT=1` | Disables the resolve→texture link |
| `GEARS_DRAW_RESOLVE_BLIT=1` | Blit instead of the compute resolve — cannot apply the exponent bias or the red/blue swap |
| `GEARS_DRAW_RESOLVE_SCALE=<float>`, `GEARS_DRAW_RESOLVE_NOSWAP=1` | Force the resolve's scale / suppress the swap. Together they are the compute resolve's **acceptance test**: at scale 1.0 with the swap off it must reproduce the blit byte for byte |

## Retired

| Knob | What happened |
|---|---|
| `GEARS_DRAW_RT=1` | **Gone.** The resolve→texture link is ON by default; `GEARS_DRAW_NORT=1` disables it. The old name lingered in scripts and evidence lines long after the code stopped reading it, so runs were passing a variable that did nothing — which is exactly what this registry exists to prevent |
| `GEARS_DRAW_FRAME=1` | **Gone.** Whole-frame rendering is what the runtime does; there is no arm in which the guest's draws are executed and none of them are shown. `GEARS_DRAW_FRAME_AT`/`_COUNT` still bound which frames |
| `GEARS_DRAW=1` | **Gone** with the one-shot hot-pair renderer it fired. That path built its own pipeline for one known vertex/pixel pair; the whole-frame renderer issues the same draw as one of the frame's draws, through the code that actually ships. Two renderers meant the milestone one drifting untested |
