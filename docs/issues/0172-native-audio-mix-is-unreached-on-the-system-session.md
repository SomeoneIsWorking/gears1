---
id: 172
title: Native audio mix is unreached on the system session
status: open
symptom: the product installs the 0x825F7B40 audio-mix override, but a 150 s run through the menus and Act 1's opening scene records 0 calls
tags: audio,native-override,x360port
state_items: S004,S009
created: 2026-09-22
updated: 2026-09-22
---

## Finding

`0x825F7B40` is called only by `bl` instructions at `0x825F793C` and
`0x825F7A9C`, both inside the function near `0x825F712C`. That function has no
direct callers, so the title reaches it only through a pointer: plausibly a
voice or speaker-configuration variant that Xenia's audio system never selects.
Dispatch itself works: an override on the draw entry `0x8222CFF8` counted 211
calls in 8 s of the same session.

## Required work

Find the pointer that selects the enclosing function (a vtable or callback
table written at runtime) and the condition that installs it. Then either show
that a supported configuration reaches it, or move the native audio override to
the mix the title actually runs under Xenia's audio system. The differential
qualification of S004 stays valid either way; it proves the kernel, not that the
product runs it.
