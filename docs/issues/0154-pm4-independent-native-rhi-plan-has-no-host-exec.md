---
id: 154
title: PM4-independent native RHI plan has no host execution
status: investigating
symptom: Native RHI plan accepts semantic frames but does not execute them
tags: performance,native-rhi,frontend,render
created: 2026-08-28
updated: 2026-08-29
---

The PM4-independent plan boundary is now wired to the live semantic stream, but it only validates and logs ordered commands. The compatibility renderer still owns output and no native host backend consumes the plan. A live resolve-observer false negative was fixed: the retained body can replace the command-buffer allocation behind device `+0x28` when its `+0x30` limit is crossed, so a lower post-call pointer is a valid transition. The bounded transition-aware search now preserves the existing packet checks; a Clang headless walk crossed the previously failing region through frame 2280 without a resolve refusal. The remaining gap is native resource, pipeline, resolve, and present ownership against a complete parity contract. Keep the retained body and compatibility arm callable while the backend is built and parity evidence is collected.
