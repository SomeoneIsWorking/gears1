---
id: 161
title: Gears 1 probe subsystem is a shared-looking 2,179-line monolith
status: resolved
symptom: runtime/guest_probes.cpp contains dozens of exact Gears 1 guest addresses, overrides, linker/checkpoint diagnostics, render-ring probes, and title crash policy in one 2,179-line shared runtime owner.
tags: architecture,title-boundary,probes,structure,gearsue3
created: 2026-08-30
updated: 2026-08-30
state_items: S001,S003
---

## Root cause

Probe overrides accumulated in one file while Gears 1 was the only conformance target. The responsibilities are exact-title diagnostics, not shared engine behavior, and the file now combines three independent state domains: fatal/serialization probes, render-ring/lifetime probes, and linker/checkpoint probes.

## Required resolution

Move all probe overrides under runtime/titles/gears1 and split them at the existing responsibility boundaries so every new source remains below the structure cap. Introduce one narrow title-specific cross-module header, remove the obsolete shared-looking file and its legacy ratchet, and update the exact indirect-call adapter to consume the real header instead of private forward declarations. Preserve the current override symbols and measured headless behavior; do not change or suppress any probe policy while moving ownership.

### Resolution (2026-08-30)
Moved all exact Gears 1 probe overrides under runtime/titles/gears1 and split fatal/serialization, render/lifetime, and linker/checkpoint ownership into 626-, 720-, and 884-line sources behind one title-private state/report header. Removed runtime/guest_probes.cpp and its 2,179-line structure exception. Clang product link, targeted clang-tidy, exact 30-wrapper/super-call set comparison, 92 non-quality CTests, and a 900-frame headless run preserved pool, ring/fence, and FArchiveAsync probe behavior.
