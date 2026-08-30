---
id: 162
title: Shared HLE D3D diagnostics owner contains exact Gears 1 wrappers
status: resolved
symptom: runtime/hle_d3d.cpp mixes reusable frame-report routing with eleven exact Gears 1 guest function overrides, queue offsets, addresses, and historical diagnostic policy
state_items: S001,S003
tags: architecture,title-boundary,hle,d3d,diagnostics,gearsue3
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The first title's diagnostic probes were implemented directly in the shared runtime because no
other title adapter existed. As a result, `vd_null_gpu.cpp`'s title-neutral frame boundary calls
functions whose implementation also owns Gears 1 entry points, worker-object layout, queue fields,
and retained-body wrappers. Another title cannot compose the command processor without linking that
exact policy.

## Required resolution

Keep only a tested, title-neutral diagnostics routing contract in `runtime/hle_d3d.*`. Move all
eleven exact wrappers, queue/replay state, guest offsets, and Gears-specific configuration behavior
under `runtime/titles/gears1/`. Product composition may install one complete callback set and must
refuse partial or duplicate installation; an unconfigured shared runtime is an explicit no-op.
Preserve every override and super-call symbol and verify the enabled headless census path as well as
the normal disabled path.

### Resolution (2026-08-30)
Split the owner at the title boundary: runtime/hle_d3d.* now contains only a complete-callback router with unconfigured no-op and partial/duplicate refusal, while runtime/titles/gears1/hle_d3d.cpp owns all eleven exact wrappers, queue/replay layout and state, guest addresses, and diagnostic configuration. The production router test, Clang build, exact symbol comparison, 93 non-quality CTests, full 128-unit clang-format/tidy gate, enabled 960-frame headless run with live probe/queue output, and disabled 540-frame headless run with no HLE output all pass.
