---
id: 149
title: Native RHI resource reference and retirement events are absent
status: resolved
symptom: The ordered native RHI stream cannot represent AddRef, Release, or release-to-zero destruction, so resource ownership cannot be transferred safely away from retained D3D code
state_items: S003, S004
tags: performance,native-rhi,resource,lifetime,retirement,re,gears1
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

The semantic event schema stopped at draw/binding/resolve/present, even though the retained D3D
resource ABI performs its ownership transition in `0x8222E868` and `0x8222E8E0`. Without a typed
reference operation and independently checked returned count, a native owner could not distinguish
the hot atomic-only path from the zero-to-one backing-object and one-to-zero destructor boundaries.

## What was tried / dead ends

Treating all references as a single native decrement/increment would erase the boundary semantics.
The retained bodies prove that zero-to-one AddRef recursively retains a flagged backing resource,
while one-to-zero Release recursively releases it and calls `0x8222E2C8`. Those cases remain on the
retained path until their complete ownership effects are represented and compared.

## Resolution

The ordered stream now records AddRef/Release requests, object flags/type/backing identity, previous
count, retained/native result, and release-to-zero retirement. The shared
`rhi_resource_reference.*` owner performs a big-endian sequentially consistent CAS only when the
transition cannot cross either semantic boundary. The Gears 1 wrapper defaults eligible calls to
native, exposes retained and alternating A/B controls, and falls back to the original body at a
boundary.

A concurrent focused test drives the shipping atomic implementation. A headless alternating run
observed 12,000 calls with zero semantic errors and no boundary fallback; at that point native mean
execution was about 51 ns versus 72 ns retained. A longer default-native run through frame 2940
matched 103,187/103,187 resource calls with zero missing results, mismatches, or retirements.
Release-to-zero remains an explicit gap in S003/S004 rather than being claimed here. Static
inspection now grounds the adjacent wrapper constructors `0x8222EA18` (owned backing) and
`0x8222EB78` (wrapped backing), and the semantic stream records their post-call wrapper words when
they execute. The current headless menu walk reached frame 1440 with zero construction events, so
the contracts are not live-covered and no native construction or destruction owner is authorized.
