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
| `GEARS_DRAW_FRAME` | Render whole frames with the guest-draw backend |
| `GEARS_DRAW_FRAME_AT=N` | First guest frame to render. Loading frames carry 2–3 draws; the Act 1 scene phase holds 700+ |
| `GEARS_DRAW_FRAME_COUNT=N` | Render N frames then stop; `0` renders every frame from `_AT` onward (the live path) |
| `GEARS_DRAW_FRAME_REPORT_EVERY=N` | Census + screenshot every N rendered frames (~40 ms each, so a visible hitch by design) |
| `GEARS_DRAW=1` | Fire the one-shot hot-pair draw instead |
| `GEARS_DRAW_DIR=<dir>` | Where screenshots, dumps and diagnostics are written |
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
| `GEARS_REPLAY_MIRROR_MB=N` | *(frame_replay)* Override the guest-memory mirror size |
| `GEARS_REPLAY_DUMP_SHADERS=<dir>` | *(frame_replay)* Write every distinct microcode blob, named by hash, for `tools/xenos_translate --raw` |

## Diagnostics — dumps and censuses

| Knob | Meaning |
|---|---|
| `GEARS_DRAW_DIAG=<path.tsv>` | **The per-draw table.** One row per draw joining what it was, what it did (pipeline statistics) and every piece of state that can zero it, with a `verdict` naming the stage it died at. Start here |
| `GEARS_DRAW_RESOLVE_DUMP=1` | Write every resolve target to a PPM with its max colour component — what the guest's post passes actually sample |
| `GEARS_DRAW_TEX_DUMP=1` | Write each decoded guest texture; `tools/decode_bc.py` turns one into a PNG |
| `GEARS_DRAW_VDUMP=N` | Dump draw N's first vertices at the shader's own stride. N is the `draw` column of the diag table |
| `GEARS_DRAW_STATS=1` | Per-draw pipeline statistics |
| `GEARS_DRAW_CENSUS=1`, `GEARS_DRAW_FRAME_LIST=1` | Per-run and per-draw censuses |
| `GEARS_DRAW_FRAME_STEP=N` | Checkpoint image every N draws |
| `GEARS_DRAW_VALIDATE=1` | Vulkan validation layers |
| `GEARS_SHADER_CAPTURE=1`, `GEARS_CONST_DUMP=1` | Capture bound microcode / the register file |

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
