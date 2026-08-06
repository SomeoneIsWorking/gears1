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
| `GEARS_DRAW_PS_CONSTS=<ps hash>` | Print the pixel float constants a named shader actually received, as numbers. Works on its own — it pulls in the per-draw listing for the draws it names, and only those. A hash that matches NO draw in the frame says so with the draw count, because that and "the constants are all zero" otherwise both print nothing. `psconst 9/3 nz` says three of nine are non-zero and cannot say WHICH — and for a pass ending in a scale, "the scale is 0.9" and "the scale is 0" are a frame and a black screen. Prints the **raw bits** alongside each float: `-nan` as a word says nothing, while `ffc00000` (an invalid operation) and `ffffffff` (uninitialised memory) point at completely different bugs. It found catalog #73 |
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
| `GEARS_DRAW_AB_UNTILE=1` | Alternates the tiling collapse **frame by frame inside one run**, so both arms share the run's thermal state, scene, caches and allocator. Separate runs cannot resolve a difference this size (`runtime/frame_ab.h`). Result: **not resolved on any capture** — the collapse removes a quarter of the draws and no measurable time (C008). Mutually exclusive with `GEARS_DRAW_AB_CENSUS`; enabling both is refused loudly, because they alternate independently and record the same frame cost |
| `GEARS_DRAW_TILED=1` | **Puts the console's predicated EDRAM tiling back.** Collapsing it is now the DEFAULT — a renderer that is native does not replay the command buffer once per EDRAM tile. The 360 does it because a 1280x720 colour+depth surface does not fit in 10 MiB; a host target has no such budget. Collapsing removes 174 of 348 base-pass draws on `courtyard`, 185 of 370 on `bright`, 193 of 389 on `play_v2`, and the image is bit-exact against the tiled path on three of four captures (claim C007). It is **not** a performance win (C008). Use this knob to A/B or bisect against the faithful path |
| `GEARS_DRAW_SPV_DUMP=<dir>` | **The TRANSLATED module, as the runtime built it for each draw's modification key** — `<vs\|ps>_<hash>_mod<modification>.spv`. Writing a native pass means implementing the translator's interface exactly (descriptor bindings, block sizes, image dimensionality, interpolator locations) and getting any of it wrong is not a validation error — it samples a different image and still draws a plausible picture. The only modules on disk before this were the offline ones in `scratch/shaders/bound_out/`, translated with **no** modification key, so they carry no interpolator inputs and a colour write mask of zero. Always the translated module, never a native substitute: dumping our own output would let a native pass verify against itself |
| `GEARS_REPLAY_DUMP_SHADERS=<dir>` | *(frame_replay)* Write every distinct microcode blob, named by hash, for `tools/xenos_translate --raw` |

## Diagnostics — dumps and censuses

| Knob | Meaning |
|---|---|
| `GEARS_DRAW_DIAG=<path.tsv>` | **The per-draw table.** One row per draw joining what it was, what it did (pipeline statistics) and every piece of state that can zero it, with a `verdict` naming the stage it died at. **Resolves are rows too** (`prim_name=resolve`, with `resolve_dest`/`resolve_src`/`resolve_dst`, and `resolve_swap_rb`/`resolve_scale` for what the guest asked the copy to DO to the colour — the red/blue swap is per-resolve, and without it in the table "which buffer is swapped relative to which" can only be answered by toggling a knob and diffing images, which is how catalog #62 acquired a wrong conclusion) — they used to be skipped, which made the frame's UE3 pass boundaries invisible. Start here, then `tools/pass_structure.py` for the pass attribution |
| *(no knob)* | Every frame reports **`frame texture signs`**: how many texture bindings ask for a signed or gamma component, which this renderer currently reads as plain unsigned. `0 of N` is printed with its denominator so the negative is distinguishable from nobody looking (`catalog.py show 69`) |
| `GEARS_DRAW_SURFACE_RANGE=1` | **The range of a SURFACE, per channel, at end of frame** — and note its max is a ONE-PIXEL statistic, which twice sent this investigation the wrong way (`catalog.py show 62`): a dark frame with one lamp in it and a bright frame have the same max. Quote a percentile from `tools/frame_stats.py` for any brightness claim. Also prints WHERE the maximum is, as a ready-to-paste `GEARS_DRAW_PIXEL_TRACE=x,y` — a range is something to look at next, and looking means aiming the trace at a coordinate that was previously found by guesswork — min, max, mean, and how many pixels have a colour channel above 1.0. Every other range this renderer reports comes from a resolve DESTINATION, which is the wrong side of the question whenever a pass renders wrong: `catalog.py show 81` needed "does surface 0x2d0 exceed 8.0" and had to infer it backwards through a resolve's exponent bias. The above-1.0 count is the point — an HDR surface that never exceeds 1.0 is the specific thing worth noticing, and a max alone can be one stray pixel. REFUSES, loudly, on a host format it cannot decode rather than reading the bytes as something they are not |
| `GEARS_DRAW_RESOLVE_DUMP=1` | Write every resolve target to a PPM — what the guest's post passes actually sample — and log its true **range and non-zero count**, plus **per-channel means and the R/G and B/G ratios**. The range alone is a max over all three channels and so reads identically whether one channel is short; the ratios are what turned catalog #62's frame-wide "red is 78% of green" into a red/blue swap, by showing two targets carrying the same three means with R and B exchanged. A target whose green sums to zero says it has no ratio rather than printing one, and NaN samples are counted per channel rather than quietly dropped from the mean. The PPM alone cannot be trusted for non-colour targets: it clamps to [0,1] at 8 bits, so a signed sub-unit buffer (the motion-blur velocity target) writes as pure black. The log line says so when that is the case (`catalog.py show 66`) |
| `GEARS_DRAW_TEX_BINDS=<ps hash>` | **What a named pixel shader actually samples.** One line per texture binding: fetch constant, base address, dimension, `exp_adjust` (with the multiplier it means, since that is the sampling side of a resolve's `copy_dest_exp_bias`), and which of the three sources served it — this frame's resolve target, a guest texture, or a stub. The frame report only counts bindings by KIND across the whole frame, which cannot answer "this one pass renders black, what is it reading". It is how `catalog.py show 81` established that the bloom bright pass reads real data and still writes zero |
| `GEARS_DRAW_TEX_DUMP=1` | Write each decoded guest texture; `tools/decode_bc.py` turns one into a PNG |
| `GEARS_DRAW_VDUMP=N[,N…]` | Dump those draws' first vertices at the shader's own stride. N is the `draw` column of the diag table. Takes a list so two draws can be compared from one run |
| `GEARS_DRAW_VS_CONSTS=N[,N…]` | **The per-instance transform.** The VERTEX float constants those draws received, as numbers and raw bits. `GEARS_DRAW_PS_CONSTS` keys on a shader HASH and so cannot separate repeated instances of one mesh — which is exactly the case this answers: draws sharing a shader pair, an index count and byte-identical raster state where only some survive clipping differ only here. It refuted catalog #74 |
| *(no knob)* | Both draw-index knobs above report **what they selected, with the denominator** — `matched 2 of 726 draws offered (diag indices 0..742 this frame)`, warning when an index falls outside the frame or a token was not a number. `if (index == want) print()` prints nothing when the index names no draw, and nothing is exactly what a draw with nothing to show prints |
| `GEARS_DRAW_SURFACE=<hex>` | Pins the probes to one EDRAM surface. For the pixel trace this means sampling THAT surface after **every** draw, whatever is bound — without it, samples taken while another surface is bound are dropped and a change that happens across a surface switch is attributed to the next sampled draw, which is several draws late. Checkpoints still only dump when it is the bound surface |
| `GEARS_DRAW_PIXEL_TRACE=<x>,<y>` | **Which draw painted this pixel.** Copies one texel after *every* draw — uncapped, in the surface's own HDR format — and prints only the draws where it CHANGED, each named with its diag draw index and pixel-shader hash. `GEARS_DRAW_FRAME_STEP` cannot answer this: it is capped at 48 images and reads back through an 8-bit blit, so on a base pass whose target is mostly above 1.0 every pixel of interest reads 255 and no two draws can be told apart. Found catalog #74 |
| `GEARS_DRAW_STATS=1` | Per-draw pipeline statistics |
| `GEARS_DRAW_CENSUS=1`, `GEARS_DRAW_FRAME_LIST=1` | Per-run and per-draw censuses |
| `GEARS_DRAW_FRAME_STEP=N`, `GEARS_DRAW_FRAME_STEP_FROM=M` | Checkpoint image every N draws, starting at draw M. **M counts ISSUED draws, not the `draw` column of the diag table** — those are the guest's indices and are larger whenever draws are dropped or collapsed (act1 issues 527 of 737, so every guest index above 527 names a checkpoint that can never fire). Passing a guest index used to produce ABSOLUTE SILENCE, which reads as "the checkpoints found nothing"; it now says no checkpoint was taken, what M was, and how many draws the frame issued. **This is how a defect is attributed to a draw.** Capped at 48 images and it says how many it dropped — without `_FROM` the cap always lands on the first 48 steps, so a late-frame defect (UI, post) can never be reached. Each line names **which EDRAM surface it dumped**: a frame switches targets several times, and without that a checkpoint dropping from 900k non-black px to zero reads as "something wiped the frame" when the target merely changed to a small bloom buffer (`catalog.py show 73`). A checkpoint asked for right after a **resolve** used to vanish with no line at all (the open target is nulled when the pass ends) — and those are the ones that say whether a pass's output survived to its resolve; it now falls back to the last opened surface and, when it still cannot take one, says so (`catalog.py show 73`) |
| `GEARS_DRAW_NO_TEX_SIGNS=1` | **Control arm.** Leave `texture_swizzled_signs` at zero, so every texture fetch takes the unsigned, undecoded path — the behaviour before the sRGB decode was implemented. On a gameplay frame that is 566 of 834 bindings reading a gamma texture as linear (`catalog.py show 69`) |
| `GEARS_DRAW_FORCE_LDR=1` | **Control arm.** Collapse a reinterpreted surface's host image to 8-bit UNORM, to ask what the fixed-point render target's source-colour clamp would have done. Destroys every HDR pass on that surface, so it answers one question and breaks the frame (`catalog.py show 68`) |
| `GEARS_DRAW_AB=<KNOB>` | *(frame_replay)* **The interleaved render comparer — root-causes a rendering difference in one command.** Renders the frame TWICE IN ONE PROCESS, once with `GEARS_<KNOB>` unset and once with it set, and reports the FIRST draw at which the two diverge, with both arms' statistics and the identical draws before it. Each arm gets a clean renderer (`ResetRendererForComparison`) because most knobs are consumed while BUILDING persistent state — a surface's host format is chosen once, at creation, so without the reset arm B inherits arm A's surfaces and the knob appears to do nothing. **It refuses rather than reports when the arms issue different draws** (the tiling collapse does: 550 rows against 724), because a row-by-row comparison would then compare two different draws. And when the arms agree it says it cannot distinguish "the knob changes nothing" from "the knob never applied" |
| `GEARS_DRAW_TRACE_ALL=<path.tsv>` | **The render comparer.** One row per issued draw: what the draw WAS (diag index, surface, pixel shader) and a hash plus per-channel max/mean of a 32x18 thumbnail of the surface AFTER it. Run two arms under different knobs and `tools/render_diff.py a.tsv b.tsv` names the FIRST divergent draw. Honours `GEARS_DRAW_SURFACE` to pin one surface. A thumbnail rather than one texel because a single pixel cannot see a change elsewhere; a thumbnail rather than the whole surface because 800 full readbacks a frame is not a tool anyone runs — so it cannot resolve a difference smaller than one of its texels, and says so |
| `GEARS_DRAW_PASS_LOG=1` | Every render-pass BEGIN: the draw it happened at, the surface, the host format, the framebuffer, and whether it is the CLEAR pass or the LOAD pass. "Which pass, which framebuffer, clear or load" is not derivable from any other line, and the pass begin was the last surviving candidate for a change that no draw could account for (`catalog.py show 62`) |
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
| `GEARS_DRAW_PS_CONST_SET=<pshash>:<i>=<x>,<y>,<z>,<w>` | One packed pixel float constant, replaced (`;`-separated for several). Answers "is the picture wrong because of THIS number?" by substituting a working capture's value. **Never a fix** — the number comes from the guest, so a wrong one is a defect on the CPU side. It is what proved catalog #73: forcing c7 and c8 took a wholly black frame to 99.4% non-black, and either one alone left it black |
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
