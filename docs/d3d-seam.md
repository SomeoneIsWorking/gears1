# Gears 1 graphics ABI facts

This document retains only executable-address and data-layout facts useful to the
future Xenia-fed title adapter. They are not current dispatch hooks.

## Exact Gears 1 entry points

| Address | Recovered role |
|---|---|
| `0x82220858` | Texture binding; copies a six-dword fetch descriptor into device state. |
| `0x82229B28` | Color-write gamma/sRGB state setter; maps format pairs 2/10 and 3/12 and marks dirty bit 37. |
| `0x82222808` | Pixel-shader setter; device object field `+0x3080`. |
| `0x82222B98` | Vertex-shader setter; device object field `+0x3084`. |
| `0x8222CFF8` | Transient auto-index draw. |
| `0x8222D4F8` | Transient DMA-indexed draw. |
| `0x8222DA48` | Bound-stream auto-index draw. |
| `0x8222DE50` | Bound-buffer DMA-indexed draw. |
| `0x822346A8` | Shader-state flush that emits ordered sequencer loads. |
| `0x82235528` | Logical resolve owner. |
| `0x8223E3E0` | Lower presentation boundary before `VdSwap`. |
| `0x82487510` | Device-state reset; clears 16 vertex-stream object slots at device `+0x2F9C` and strides at `+0x2FE0`. |

## Shader container

The big-endian magic is `0x102A11tt`, where `tt=0` is pixel and `tt=1` is
vertex. `+0x04` is the header size, `+0x10` points to D3D metadata, and `+0x18`
points to shader information whose first two words are constant-block and Xenos
microcode byte sizes. The data region is `[constants][microcode]`; microcode size
is a nonzero multiple of the 12-byte Xenos instruction slot.

User-image measurements found 425 distinct package microcode payloads and all 425
passed Xenia SPIR-V translation and validation. Runtime-bound vertex microcode has
fetch strides patched into it, so package templates are not authoritative bind
identity. Concrete identity belongs to the `IM_LOAD` or `IM_LOAD_IMMEDIATE`
payload selected by the command stream.

## Resource facts

Texture objects contain a six-dword Xenos fetch descriptor. Shader constants use
the ALU constant register range. Vertex and index buffers use physical guest
addresses with title-specific endian rules. Render targets use Xenos EDRAM surface
state and resolve into guest memory before presentation.

All facts above must be validated against the exact authenticated executable after
the Xenia executor exists. Until then they are adapter inputs, not proof of a live
native renderer.
