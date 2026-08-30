---
id: 159
title: Shared indirect-call owner contains Gears 1 diagnostic policy
status: resolved
symptom: runtime/guest_indirect_call.h and .cpp hardcode the exact Gears 1 streaming return address, caller ranges, and probe reporters, so the shared GearsUE3 checked-call boundary cannot be reused by another title adapter.
state_items: S003
tags: architecture,title-boundary,indirect-call,gearsue3
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The generic checked indirect-call path grew Gears 1 crash investigation policy directly inside its hot-path header and failure reporter. The checked target validation and attributable abort are shared engine behavior; the return-address selector and linker/streaming probe context are exact-revision policy.

## Required resolution

Keep one generic checked-call implementation and generic fatal report. Inject the exact title observation at compile time so successful calls gain no function-pointer dispatch, and let the exact title adapter supply failure context through a narrow callback. Preserve the invalid-target abort and add positive/negative controls for the Gears 1 return selector. Do not clamp the bad table index or continue after an invalid call.

### Note (2026-08-30)
Resolved: runtime/guest_indirect_call.* now owns only generic code-range/alignment/table validation and fatal reporting through a nullable title-context seam. runtime/titles/gears1/guest_indirect_call.* owns the exact 0x823EDB50 observation selector, exact caller ranges, issue #50 table report, and Gears probe bridges; its forced header delegates every generated call to the shared validator with no successful-path callback dispatch. The focused selector/dispatch controls pass, all 191 generated translation units compile through the wrapper, all 90 non-quality tests and the 646.59-second cpp_quality gate pass, and a headless run reached 300 frames. Invalid calls still abort; the table index is never clamped or continued.
