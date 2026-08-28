---
id: 154
title: PM4-independent native RHI plan has no host execution
status: investigating
symptom: Native RHI plan accepts semantic frames but does not execute them
tags: performance,native-rhi,frontend,render
created: 2026-08-28
updated: 2026-08-28
---

The PM4-independent plan boundary is now wired to the live semantic stream, but it only validates and logs ordered commands. The compatibility renderer still owns output and no native host backend consumes the plan. Root cause of the remaining gap: native resource, pipeline, resolve, and present ownership has not yet been implemented against a complete parity contract. Next bounded candidate: logical resolve 0x82235528, after extending its semantic controls and proving same-binary A/B output, PM4 absence, and negative controls. Keep the retained body and compatibility arm callable.
