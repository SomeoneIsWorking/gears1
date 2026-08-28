---
id: I066
kind: instrument
status: trusted
created: 2026-08-29
---

## Instrument

`runtime/native_rhi_vulkan_resolve.*` plus `test_native_rhi_vulkan_resolve`

## Validated by

The focused headless test creates two host-owned `VK_FORMAT_R8G8B8A8_UNORM`
images, records a real `vkCmdCopyImage` through the production native resolve
operation, submits it on a Vulkan graphics queue, and checks the copied pixel
rectangle through a host-visible readback buffer. The same test supplies an
unsupported operation flag and requires explicit refusal.

## Known failure modes

This validates one-to-one colour image copying only. It does not establish
Xenos format conversion, swizzle, multisample resolve, guest resource
allocation, native draw production, live-plan consumption, or state/pixel
parity. The operation must remain disconnected from the live stream until a
native producer owns both images and those contracts are independently proven.
