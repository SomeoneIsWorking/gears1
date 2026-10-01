# Project state

`verified` = exercised by the cited test or recorded real-title evidence;
`partial` names the gap; `missing` = no product implementation exists.

The RE evidence chain behind these states is in `docs/re-frontier.md`.

| ID | Capability | State | Evidence or gap |
|---|---|---|---|
| S001 | Exact Gears 1 image identity | verified | `config/titles/gears1.toml`, `tools/title_identity.py`; container and normalized-image hashes fail closed. |
| S002 | Bounded user-image provisioning | partial | `./run.sh` resolves the disc, extracts it, and refuses any `default.xex` digest but the profile's. Gap: no no-terminal first-run screen, no packaged delivery. |
| S003 | Executor-independent native renderer and RHI contracts | partial | Focused native frame/draw/resolve/resource tests build without a CPU executor. Gap: no live Xenia-fed native frame. |
| S004 | Native Gears 1 audio mix (`0x825F2D40`) | verified | `tests/test_gears1_real_leaf.cpp` A/B against the original body; `--verify-audio-mix` compared 33M live calls with 0 disagreements. |
| S005 | Native notified GPU ticket wait | partial | `runtime/gpu_ticket_wait.*` keeps the host contract. Gap: guest-address dispatch through `x360port` is missing. |
| S006 | Xenia-backed `x360port` execution boundary | partial | `gears1_dynarec_boundary` crosses JIT to typed import to native code; override/original-call/invalidation and a bounded fallback are contracted upstream. Gap: fallback ISA coverage and title write/cache-control semantics. |
| S007 | Gears 1 leaf/import/override discriminator | partial | `test_gears1_real_leaf` executes real leaf `0x8222E868` and its nested call, with typed import refusal and 236 resolved imports. Gap: vibration and remaining device semantics. |
| S008 | Bounded interpreter fallback | partial | Unsupported opcode refused without host code; a narrow `lswi`/immediate/big-endian/compare/branch subset executes with counted entries. Gap: complete PPC ISA, control flow, real-image fallback. |
| S009 | Representative interactive Gears 1 gameplay | partial | `tools/combat_route.py` plays from the first firefight to `WarCheckpoint_3` closed-loop over the control channel, with game time 0.9995 of wall time and 0 translation failures. Gap: past `WarCheckpoint_3` the door breach is not cleared; no later act or level load. |
| S010 | Apple Silicon macOS A64 execution | partial | The asset-free A64 contract passes on macOS CI (`MAP_JIT` code cache, FDE on Darwin, host-page protection). Gap: no guest title, product host, package, or performance there. |
| S011 | Android arm64-v8a A64 execution | missing | Needs an APK/runtime owner; nothing exists. |
| S012 | Complete native RHI frontend | missing | Existing native pieces do not bypass guest command construction or prove frame parity. |
| S013 | Native 8.33 ms / 120 fps renderer budget | partial | Headless gameplay walk holds 120 presents/s at p50 8.4 ms through the menus and cell block; the path junction holds 80-114 and the yard firefight 19-114 depending on host load. Gap: the guest render thread saturates at the junction. |
| S014 | Gears 2, Gears 3, Judgment conformance | missing | Gears 1 first; unknown revisions must refuse. |
| S015 | No generated guest-source product | verified | Build graph, launcher, tests, tools, and docs carry no generated corpus, function map, or offline translator; `tools/check_migration_boundary.py` refuses new ones. |
| S016 | Independently authored shared UE3/Xbox contract | partial | `shared/x360ue3` at its pinned revision, consumed by `gears1_ue3_contract`. Gap: not yet composed with the authenticated real-image runtime. |
| S017 | Asset-free native/JIT boundary CI | partial | `.github/workflows/dynarec-boundary.yml` runs the Gears discriminator on Linux, Windows, and macOS arm64 with the exact pins. Gap: hosted runs pending, including the Windows `tabulate` fix awaiting a fork decision. |
| S018 | PC keyboard and mouse controls | partial | W/A/S/D, pointer aim, click fire/aim; `gears1_desktop_controls` covers bindings, cancellation, and remote-pad arbitration. Gap: no agent run exercises GTK capture; bindings are fixed. |
| S019 | Campaign checkpoints save and resume | verified | One local `Player` profile under the OS user-data root writes `default_checkpoint.sav`; the next run offers Continue Campaign. |
| S020 | Native engine reads cooked packages | verified | `test_engine_package` covers whole-file, chunk, and plain forms plus refusals; LZO1X agrees with FFmpeg byte for byte. |
| S021 | Native engine reads serialized properties | verified | `test_engine_object`; 12.5M properties read with 0 failures across the disc. |
| S022 | Native engine decodes assets and renders a level | partial | `gears_level_render` drew SP_Adams_S08_MainRoom with 721/728 textured section draws. Gap: terrain, skeletal meshes, lighting/lightmaps, an interactive window, and unmeasured face winding (issues 0175-0180). |

Comparison baseline: retail Xbox 360 Gears in an emulator. User-visible deltas —
keyboard and mouse beside the gamepad (S018); a signed-in local profile with
working checkpoints (S019); presentation uncapped from the console's 30 Hz to 120 Hz
with the game's clock still on the host clock (S013); 1440p by default instead of
720p; no-terminal setup, packaging, and rebinding are not delivered (S002, S018).

Current focus: Gears 1 gameplay past `WarCheckpoint_3` on `tools/combat_route.py`
(issue list `docs/issues/`), and the 120 presents/s budget at the path junction and in
the yard firefight.
