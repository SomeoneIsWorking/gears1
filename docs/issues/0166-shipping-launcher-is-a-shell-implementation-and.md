---
id: 166
title: Shipping launcher is a shell implementation and not a fresh-clone initializer
status: resolved
symptom: run.sh contains configuration, build, logging, cleanup, and Gears 1 menu-walk policy, invokes ambient CMake directly, and only runs when extracted and recompiled title outputs already exist
state_items: S006,S008
tags: launcher,bootstrap,python,provisioning,title-boundary,architecture,gearsue3
created: 2026-08-30
updated: 2026-08-31
---

## Root cause

The launcher predates the locked Python project and the GearsUE3 multi-title boundary. Configuration,
asset refusal, CMake policy, logging/FIFO lifecycle, process cleanup, and launch environment therefore
accumulated in 150 lines of shell. Exact Gears 1 navigation is sourced into that shipping interface
from `tools/menu_walk.sh`, while extraction, title identity, recompilation, and generated-profile
validation remain separate maintainer commands. As a result, `./run.sh` is neither a slim stable shim
nor the fresh-clone product initializer: it escapes the locked interpreter, can configure a reduced
target when required native dependencies are absent, and cannot produce its own title module from the
user's disc image.

The menu-walk authority also has four non-launcher shell consumers. Copying its values into a new
Python bootstrap, parsing shell as data, or retaining a shell wrapper around Python would create a
second authority rather than fix the ownership defect.

## Resolution

`run.sh` is now a four-line `uv run --frozen` shim. `bootstrap.py` composes the tracked
Gears 1 profile with focused Python owners for strict machine-local configuration, prerequisites,
content-addressed disc provisioning, product builds, logging, and child lifetime. A selected image
is validated before any submodule or compiler work starts; unknown revisions refuse. The old shell
scenario entry points are now Python callers of the same profile authority and the shell copies are
removed. Generated PPC headers have an explicit scoped refresher for a locally stale generated tree;
normal provisioning regenerates them from the title.

Evidence: 24 focused bootstrap tests cover profile/environment ownership, cold synthetic preparation,
fail-before-build input refusal, process lifetime, and header refresh. A Clang combined build compiled
all 191 generated PPC units and passed 101/101 CTest checks, including the source-structure,
distribution, and C++ quality gates. XenonRecomp and Xenia dependency fixes are pinned to portable
GCC-capable revisions; GCC compiled their affected production layers before the checked generated
module boundary.

## Required resolution

Reduce `run.sh` to a repository-root shim that enters `uv run --frozen` and delegates to one Python
initializer. That initializer must preserve the shipping CLI and runtime exit status, use its exact
interpreter for every generator and CMake build, require the current GearsUE3 product target rather
than accepting a reduced build, and provide actionable platform package commands for missing native
dependencies. Given `GEARS_ISO`, it must initialize redistributable dependencies, extract into ignored
content-addressed storage, validate the exact title profile, generate the local recomp module, build,
and launch without Ghidra or private inputs; a non-launching preparation mode must exercise the same
cold path.

Move exact navigation schedules into one title-profile data owner consumed by Python tooling. Migrate
the affected non-launcher shell tools atomically and remove `tools/menu_walk.sh` instead of preserving
a compatibility wrapper or generated shell copy. Tests must cover missing dependencies/assets,
profile refusal, a cold isolated preparation, argument/environment propagation, logging, child
termination, and non-zero runtime status before the launcher can be called fixed.
