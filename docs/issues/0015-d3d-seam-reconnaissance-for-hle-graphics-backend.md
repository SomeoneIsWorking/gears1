---
id: 15
title: D3D seam reconnaissance for HLE graphics backend
status: open
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
Reconnaissance only; full write-up in docs/d3d-seam.md. Key facts with evidence:

API SHAPE: one global D3D device (**(0x82000868), instance 0x4015B080), flat statically-linked functions taking the device in r3 and returning it (call chaining verified in 0x8254ED40's nine chained sampler setters). No vtable on the hot path; vtables exist one layer up (UE3 render commands executed by 0x82444EF0). Deferred-state model: setters shadow state in the device + OR 64-bit dirty masks at dev+0x10/+0x18; sub_82544148 is the flush-and-draw emitter. WARNING recorded: address ranges are not layer boundaries (0x8221CBA8 is UE3's render-command ring allocator despite sitting in the D3D range -- it writes the ring block at 0x82C0CB24).

STATIC SURFACE: 201 functions in 0x82218000..0x82241000 called from outside the range; 51 called from the RHI/draw zone 0x82540000..0x82560000 (bl-scan, reproducible).

DYNAMIC FRAME PROFILE (gdb ignore-count breakpoints, ~30s of movie phase, normalised per 414 VdSwaps): SetTexture=0x82220858 at 6.2/swap (copies fetch-constant words from texture object +0x1C..+0x30 into the device fetch shadow at dev+(slot+0xBFC)*4); once-per-frame: 0x8222CFF8, 0x82221980 (ring flush), 0x82222460 (dirty helper); sampler family 0x82228998+8 siblings at 0.4/swap applied via 0x8254ED40; present chain UE3 0x824A5170 -> 0x8223E860 -> 0x8223E3E0 -> VdSwap (found via bl-scan to the VdSwap thunk 0x82AC68A4, call sites 0x8223E43C/0x8223E488). IMPORTANT: the post-load steady state calls NONE of these (all 19 breakpoints at zero over 40s) -- the title stops presenting after loading and waits; that blocker is undiagnosed and HLE will not fix it by itself.

SHADERS: container magic 0x102A1100 (BE); two built-ins embedded in the image at 0x82039878/0x82039A40 (totalSize 0x10C, carries D3D9 SM3 token 0xFFFF0300 as metadata); validation at create in the 0x82227600 region; UE3-side loaders referencing the magic at 0x82617464/0x8261D58C/0x826223E0/0x82694CD0/0x8274679C. The executable payload is XENOS MICROCODE -- shader translation (ucode -> host) is the unavoidable hard core.

RESOURCES: guest objects carry PRECOMPUTED hardware descriptors (texture objects hold the 6-dword fetch constant; draw flush emits TYPE0 to 0x4800+, ALU constants to 0x4000+, RB/PA EDRAM setup in the 0x2xxx register series -- all observed in live streams). HLE must either hook creation APIs (entry points NOT yet identified; candidates 0x82227120/0x82227000 from UE3 texture code 0x8264Dxxx) or parse fetch constants at bind time.

SIZE ASSESSMENT (blunt): first movie frame = hook layer + resource map + hand-written host equivalents of the two built-in shaders = small weeks. General scene rendering = full ucode translator + EDRAM/resolve model + remaining API surface = largest remaining subsystem, comparable to everything done so far combined. The existing LLE command processor stays as the transition scaffold; its packet knowledge maps directly onto the HLE state model.

NOT ESTABLISHED (explicit): creation-API entry points; shader container section table; vertex declaration/stream binding entries; the post-load no-present blocker; whether D3D-internal paths (e.g. the movie player's own draw at 0x8221D3A8) bypass any hookable API -- the movie draw is INSIDE D3D, so the hook must sit at/above it or the LLE CP must remain during transition.
