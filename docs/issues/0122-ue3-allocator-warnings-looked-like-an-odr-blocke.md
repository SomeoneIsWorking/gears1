---
id: 122
title: UE3 allocator warnings looked like an ODR blocker but emitted no global replacements
status: resolved
symptom: Clang warns that UE3 inline replacement new/delete declarations cannot be inline
tags: native-ue3,core,clang,link
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

The warning describes invalid inline attributes on replacement allocation declarations, but it was incorrectly promoted into an assumed link blocker without inspecting emitted symbols. Clang emits only the class-specific weak placement operators used by UE3 in the current archive; it does not emit global replacement new/delete definitions from these headers.

## Evidence

A whole-archive relocatable link combines all 58 selected Core objects successfully. Exact-symbol inspection finds no `_Znwm`, `_Znam`, `_ZdlPv`, or `_ZdaPv` definition.

## Resolution

Removed allocator ODR work from the active frontier. Preserve the warning as source-lineage information, but build the missing Linux platform services next.

## Dead end

Do not add allocator shims or patch the external allocation declarations merely to silence this warning.
