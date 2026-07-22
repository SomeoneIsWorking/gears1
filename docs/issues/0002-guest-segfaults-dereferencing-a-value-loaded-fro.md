---
id: 2
title: Guest segfaults dereferencing a value loaded from .rdata
status: resolved
symptom: SIGSEGV three guest frames in; lwz from an address loaded out of .rdata that is not a pointer (e.g. 0x93010100)
tags: loader,imports,xenonrecomp
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-07-22)
XenonUtils patches FUNCTION import thunks with a nop/blr stub but leaves VARIABLE imports (type==0) holding their raw ordinal record, which the game then dereferences. Decode: type=0, ordinal=0x193 = XexExecutableModuleHandle. Fixed in our fork: Image::importVariables records them and the runtime points each slot at real guest storage. 236 in this title.
