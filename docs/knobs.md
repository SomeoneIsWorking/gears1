# Runtime configuration

Every environment variable consumed by built first-party C++ (the product, the
retained renderer libraries, and `frame_replay`) goes through `lucent::config`,
whose `GEARS_` prefix is applied centrally. Variables that select the deleted
CPU product or its comparison wrappers do not exist. Sources kept under
`runtime/` that no target compiles read further names; a name returns here only
when a target builds its reader again.

## Product

| Names | Purpose |
|---|---|
| `GEARS_INPUT_SCRIPT` | Timed controller script (`ms:BUTTON` steps, `ms:` releases) that owns the pad until its last step; `tools/run_offscreen.py` sets it from a profile walk. |

## Native passes and semantic planning

| Names | Purpose |
|---|---|
| `GEARS_NATIVE_PASSES`, `GEARS_NATIVE_PASSES_KEEP_TRANSLATED` | Select independently authored native shader passes and retain the Xenos-translated comparison arm for offline/frame-replay evidence. |
| `GEARS_NATIVE_RHI_OBSERVE`, `GEARS_NATIVE_RHI_PLAN` | Enable executor-independent semantic collection or native frame-plan validation when a caller supplies events. They do not create guest bindings. |

## Renderer diagnostics and controls

| Names | Purpose |
|---|---|
| `GEARS_DRAW_STATS` | Bounded per-frame draw statistics. |
| `GEARS_DRAW_FRAME_LIST`, `GEARS_SKINNED_CHECK`, `GEARS_SKINNED_CHECK_LIST` | Frame and skinned-draw inspection. |
| `GEARS_DRAW_RESOLVE_DUMP`, `GEARS_DRAW_RESOLVE_DUMP_EACH`, `GEARS_DRAW_RESOLVE_DUMP_FLOAT` | Bounded resolve evidence. |
| `GEARS_DRAW_TEX_DUMP` | Decoded guest-texture evidence. |
| `GEARS_DRAW_PASS_LOG`, `GEARS_DRAW_VALIDATE`, `GEARS_DRAW_UBOCHECK` | Render-pass, Vulkan validation, and uniform-layout diagnostics. |
| `GEARS_DRAW_SURFACE_RANGE` | Restrict a diagnostic to an explicit host surface range. |
| `GEARS_DRAW_AB_CENSUS`, `GEARS_DRAW_AB_TARGET_LOOKUP`, `GEARS_DRAW_AB_TEXDIRTY`, `GEARS_DRAW_AB_UNTILE` | Offline/frame-replay same-process comparison controls for retained renderer algorithms. |
| `GEARS_DRAW_NO_TARGET_LOOKUP_CACHE`, `GEARS_DRAW_NO_TEX_DIRTY`, `GEARS_DRAW_NO_TEX_SIGNS` | Disable one renderer optimization/interpretation for a diagnostic comparison. |
| `GEARS_DRAW_NOBLEND`, `GEARS_DRAW_NOCLAMP`, `GEARS_DRAW_NOCULL`, `GEARS_DRAW_CULL_INVERT`, `GEARS_DRAW_NODEPTH`, `GEARS_DRAW_NODEPTHBIAS`, `GEARS_DRAW_NOMSAA`, `GEARS_DRAW_NOSTENCIL`, `GEARS_DRAW_NOTEX` | Disable one bounded graphics behavior to discriminate a rendering cause. Never product fixes. |
| `GEARS_DRAW_FIXEDVP`, `GEARS_DRAW_FORCE_LDR`, `GEARS_DRAW_MODE_ONLY`, `GEARS_DRAW_NOALIAS`, `GEARS_DRAW_NOREINTERP`, `GEARS_DRAW_NORT`, `GEARS_DRAW_TILED`, `GEARS_DRAW_SPLIT_DEPTH` | Select one bounded viewport, format, aliasing, resolve, tiling, or depth-model comparison. |
| `GEARS_DRAW_REINTERP_SELFTEST`, `GEARS_DRAW_RESOLVE_BLIT`, `GEARS_DRAW_RESOLVE_NOSWAP`, `GEARS_DRAW_SLATE_CLEAR`, `GEARS_DRAW_DEPTHONLY_PS` | Explicit negative/control paths that must report their activation and denominator. |

## Diagnostic self-tests

| Names | Purpose |
|---|---|
| `GEARS_STALL_SELFTEST` | Exercise both stalled and never-started watchdog outcomes. |

Launcher/provisioning variables (`GEARS_ISO`, `GEARS_GAME_DIR`,
`GEARS_BUILD_DIR`, and `GEARS_ENV_FILE`) are parsed by the locked Python
environment owner, not by C++. Tool-specific capture paths are documented by
the corresponding Python tool and do not select product execution.
