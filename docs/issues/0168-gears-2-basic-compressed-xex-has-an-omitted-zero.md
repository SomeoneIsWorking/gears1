---
id: 168
title: Gears 2 basic-compressed XEX has an omitted zero tail
status: resolved
symptom: xex-inspect refuses Gears 2 default.xex because its basic-compression blocks expand to 0x1228000 while security imageSize is 0x1230000
state_items: S006,S008
tags: xex,provisioning,title-boundary
created: 2026-08-31
updated: 2026-08-31
---

The user-owned Gears 2 XEX is basic-compressed and has two valid data/zero blocks. Their expanded size is 0x1228000; the XEX2 security header declares 0x1230000, leaving an exact 0x8000 tail. Independent loader behavior and the XEX security image size establish that the missing tail is zero-filled image space rather than a title-specific exception. Fix the shared loader generically and retain refusals for block/output overflow.

### Resolution (2026-08-31)
The shared loader now accepts a basic-compressed stream whose descriptors expand to less than the declared image size and zero-fills the remaining image tail, while retaining overflow refusal. Focused XexInspectTests pass, clang-tidy passes, and the real Gears 2 XEX now passes xex-inspect and title_identity.
