# Codemap

This map owns placement and responsibility only. Capability state is in
`docs/project-state.md`; evidence order is in `docs/re-frontier.md`.

| Owner | Current location | Responsibility | Boundary |
|---|---|---|---|
| Product launcher | `run.sh`, `bootstrap.py`, `tools/gearsue3_bootstrap/` | Resolve locked Python, user image, native dependencies, build, and launch. | Refuses by name until the pinned `x360port` exposes a gameplay executor. |
| Exact title identity | `config/titles/gears1.toml`, `tools/title_identity.py`, `runtime/title_profile.*` | Validate exact container/image hashes and title policy. | No CPU translation, generated source, or runtime cache ownership. |
| Shared Xbox 360 execution | `../../shared/x360port` at the revision pinned in `CMakeLists.txt` | Xenia `Memory`, `Processor`, `ThreadState`, `RawModule`, x64/A64 dynarecs, bounded interpreter fallback, typed imports, overrides, exits, and invalidation. | No Gears address, hash, renderer policy, or UE3 behavior. |
| Shared UE3/Xbox contracts | future `../../shared/x360ue3` | Independently authored, versioned UE3-on-Xbox interfaces proven from a live executor. | Must not exist until Gears consumes a real contract; never copies or depends on `../../shared/ue3`. |
| Gears-family native host | `runtime/` | Input, audio, kernel/device services, renderer, diagnostics, frame ownership, and executor-independent guest-memory views. | CPU execution enters only through `x360port`. |
| Gears 1 adapter | `runtime/titles/gears1/`, `config/titles/gears1.toml` | Exact addresses, ABIs, native operations, renderer bindings, and revision policy. | Other titles get separate adapters; shared owners contain no Gears 1 constants. |
| Native renderer | `runtime/gpu_*`, `runtime/native_*`, `runtime/rhi_*`, `runtime/titles/gears1/native_pass.*` | Xenos state/materialization, host Vulkan resources, semantic frame plans, and independently authored native passes. | Existing source is buildable evidence, not a live gameplay claim until fed by Xenia. |
| Xenos shader tools | `xenia_gpu/`, `tools/xenos_translate/`, `tools/system_constants/` | Offline inspection of user-supplied shader captures and register snapshots using Xenia's GPU libraries. | Measurement only; never a CPU executor or player prerequisite. Diagnostics use Lucent logging. |
| Oracle | `tools/xenia_oracle/`, `tools/oracle_lockstep.py` | Independent reference-emulator observations and bounded differential evidence. | Oracle output is evidence, not a runtime fallback or shipping dependency. |
| Migration boundary gate | `tools/check_migration_boundary.py` | Scan tracked and untracked first-party source, tool, config, and documentation paths for retired CPU-product surfaces, forbidden direct process/diagnostic access, and extra shell entry points. | Build/dependency/scratch trees are excluded; the game-authored phrase “command-list interpreter” remains valid. |
| Distribution gate | `tools/check_distribution_clean.py` | Refuse copyrighted inputs, executable/archive payloads, private-source dependencies, and unverifiable generated artifacts. | Does not replace runtime identity validation or the migration-boundary gate. |
| Architecture/quality gates | `tools/check_source_structure.py`, `tools/check_cpp_quality.py` | Source-size ratchet, format, and lint checks against the real compile database. | Maintainer verification selects Clang; the project does not reject supported user compilers. |

`run.sh` is the only shell file. All other project automation is Python.
