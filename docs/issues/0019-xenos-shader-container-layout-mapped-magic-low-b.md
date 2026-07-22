---
id: 19
title: Xenos shader container layout mapped; magic low byte is the shader type
status: resolved
symptom: cannot locate the microcode inside a 0x102A11xx shader container, or a container fails to parse
tags: gpu,shaders,container,re
created: 2026-07-23
updated: 2026-07-23
---

The magic is **0x102A11tt**, not the constant 0x102A1100 recorded earlier:
`tt` = 0 for a pixel shader, 1 for a vertex shader. The old note only ever saw
pixel shaders (both image built-ins are pixel shaders), so the type byte looked
like part of a fixed magic.

Layout (big-endian words), now mapped well enough to find the payload:

    +0x00 magic 0x102A11tt
    +0x04 headerSize, and also the offset of the data blob
    +0x08 an offset, role not identified
    +0x0C always 0
    +0x10 offset of a u32 CTAB byte size; the D3D9 constant table follows it,
          so its Version word (0xFFFF0300 ps_3_0 / 0xFFFE0300 vs_3_0) is at
          container +0x10's value + 4 + 8
    +0x14 offset of a 0x28-byte section, or 0
    +0x18 offset of the shader-info section, last thing in the header:
            info[0] constant-block byte size
            info[1] microcode byte size, always a multiple of 12
          info[2..] are register-shaped and NOT decoded

    blob = [constants][microcode], so microcode starts at headerSize + info[0].

Verification: 5443 magic-prefix hits across 768 disc files; 1290 satisfied every
structural check with zero rejections among them. The two image built-ins
(0x82039878, 0x82039A40) are the SAME pixel shader -- identical microcode,
differing only in their 16-float constant block (two YUV->RGB matrices) -- and
disassemble to exactly the movie player's converter: three tfetch2D of the
Y/U/V samplers followed by the colour matrix. That is ground truth on a shader
whose behaviour was already known.

Tooling: tools/shader_extract.py (scan + validate + dedup),
tools/xenos_translate/ (container -> SPIR-V + microcode disassembly).

Still unread: header words +0x08 and +0x14, and shader-info words 2 onward.
