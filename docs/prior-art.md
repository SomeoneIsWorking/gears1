# Prior art

External work on Gears of War (Xbox 360, title `4d5307d5`, retail `default.xex` sha256 `df1041da…2d1`, image sha256 `f61cc78e…d5c`, base `0x82000000`). Addresses were checked with `tools/ppcdis.py` against `scratch/raw/gears_image.bin`, whose sha256 matches `config/titles/gears1.toml`.

| project | URL | kind | license | target build | build match (addresses checked) | what we may take | notes |
|---|---|---|---|---|---|---|---|
| Xenia Canary game patches, `4D5307D5 - Gears of War.patch.toml` | https://github.com/xenia-canary/game-patches | Xenia memory patches (60 FPS, MSAA tiling fix, texture streaming, motion blur, widescreen/FOV) | none (no LICENSE file) | retail disc (USA, Europe), Xenia module hash `1B591620508434A2` | matched: `0x8223E7B8` `li r10,2` (patch byte `0x8223E7BB` = present interval), `0x824A4D7C` `li r10,0x2D0` (colour-buffer height), `0x8237F304` `beq cr6` (ConstrainedAspectRatio branch), `0x8245D088` function entry `mflr r12` | evidence only | Patch comments name `GUseTilingCode` `0x82BB6BBB`, `GTilingMode` `0x82BB6B53`, `InitD3DDevice` sites; treat as leads to re-measure. The `(TU5)` and `(Japan)` files target other XEXs (TU5 hash `34849A59634915F6`, present interval at `0x82250DDB`) and do not apply. |
| Xenia | https://github.com/xenia-canary/xenia-canary | emulator | BSD-3-Clause | all | n/a | already consumed through `x360port` | See `docs/xenia-reuse.md`. |
| XenonRecomp | https://github.com/hedge-dev/XenonRecomp | generic Xbox 360 static recompiler | MIT | none (title-neutral) | n/a | nothing | Generated-PPC products are retired here. |
| ReXGlue SDK | https://github.com/rexglue/rexglue-sdk | generic Xbox 360 recompilation runtime | BSD-3-Clause text, Xenia-derived (GitHub NOASSERTION) | none (title-neutral) | n/a | nothing | No Gears project built on it found. |

Not found (searched 2026-10-13): a Gears of War 1 decompilation, recompilation or native port (GitHub, decomp.dev, recompiled-games lists), public symbol maps or IDA/Ghidra databases for any Gears 1 XEX, RE wikis for the 360 executable. Leaked UE3/Gears 3 source and ports built on it ("Delta Online") are excluded, never consulted.
