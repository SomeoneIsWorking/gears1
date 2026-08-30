---
id: 164
title: Shared bugcheck handler embeds exact Gears 1 purecall site
status: resolved
symptom: runtime/kernel_misc.cpp classifies code 0 from return address 0x828D30B0 as the Gears 1 purecall terminate tail
state_items: S003
tags: architecture,title-boundary,kernel,bugcheck,gearsue3
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The shared KeBugCheck implementation acquired the meaning of one Gears 1 code/return-address pair
while that title was the only linked product. Generic Xbox kernel termination and parameter
reporting therefore became the accidental owner of an exact-revision guest call site and its
_purecall interpretation. A second title would inherit that diagnosis merely by linking the shared
kernel runtime.

## Required resolution

Keep KeBugCheck/KeBugCheckEx argument reporting and fatal process termination in the shared kernel
owner. Move exact code/return-address classifications and their descriptions into a required
linked-title policy with shared validation and lookup. One executable must link exactly one strong
policy definition; unknown sites must retain the generic diagnosis, and focused tests plus a
headless product run must preserve Gears 1 behavior.

### Resolution (2026-08-30)
Root cause fixed: runtime/kernel_misc.cpp no longer owns any exact Gears 1 code/return-address meaning. runtime/bugcheck_policy.* now validates and applies the linked title policy; runtime/titles/gears1/bugcheck_policy.cpp alone owns code 0 at 0x828D30B0 and its _purecall diagnosis. Unknown sites retain the generic message and malformed/duplicate policies refuse. Evidence: focused shipping lookup test; 95/95 non-quality CTests; 135/135 Clang quality units; Clang product link; headless run reached frame 540 with no invalid-policy diagnostic; exact address and description absent from shared code.
