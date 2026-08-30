---
id: 165
title: Shared host runtime hardcodes Gears 1 product identity
status: resolved
symptom: Shared boot, Vulkan/SDL, debug HTTP, frame timing, draw backend, and filesystem defaults expose gears1 instead of engine-owned GearsUE3 identity or the selected title profile
tags: architecture,title-boundary,host,identity,filesystem,gearsue3
created: 2026-08-30
updated: 2026-08-30
state_items: S003
---

## Root cause

The repository began as a one-title port, so shared host subsystems copied the first executable's
name into Vulkan/SDL identity, debug protocol strings, boot/timing logs, and the filesystem's default
save namespace. After the product boundary became GearsUE3, those copies remained independent
authorities. A newly linked title would therefore still present itself as Gears 1 and could silently
write saves into the Gears 1 namespace before its exact profile activated.

## Required resolution

Define the shared host-facing GearsUE3 identity once and use it in every shared presentation,
diagnostic, and renderer application string. Keep exact title/revision identity in `TitleProfile`.
Remove the filesystem's Gears 1 fallback so save access refuses until the selected profile activates
one immutable namespace. Add focused positive and negative tests, then preserve headless product
startup and the exact selected-title report.

### Resolution (2026-08-30)
Moved shared host-facing identity into `runtime/host_product_identity.h` as GearsUE3/gearsue3 and
routed boot, Vulkan/SDL, draw, and debug HTTP through it. Exact gears1/revision and save identity
remain in `TitleProfile`, and removing `GuestFilesystem`'s implicit gears1 fallback makes save access
refuse before one immutable profile activation. Focused identity/runtime tests, all 96 non-quality
CTests, the 137-unit Clang quality gate, source structure, clean-distribution tip/history, and registry
checks pass. A live headless HTTP run reported GearsUE3, gearsue3-debug, the exact gears1 revision,
and the gears1 profile save namespace; the active shared-runtime stale-literal audit found no old
product-identity copies.
