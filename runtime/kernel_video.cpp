// Display and locale queries.
#include "import_stub.h"

#include <byteswap.h>
#include <lucent/log.h>

namespace
{

// X_VIDEO_MODE as the guest expects it. The title sizes its render targets and
// picks its UI layout from these, so the values here decide what resolution the
// game will try to run at.
struct GuestVideoMode
{
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t isInterlaced;
    uint32_t isWidescreen;
    uint32_t isHiDef;
    float refreshRate;
    uint32_t videoStandard;
    uint32_t unknown1c;
    uint32_t unknown20;
    uint32_t reserved[3];
};

template<typename T>
void StoreBE(void* dst, T value)
{
    *reinterpret_cast<T*>(dst) = ByteSwap(value);
}

} // namespace

void __imp__XGetVideoMode(PPCContext& __restrict ctx, uint8_t* base)
{
    if (ctx.r3.u32 == 0)
        return;

    auto* mode = reinterpret_cast<GuestVideoMode*>(base + ctx.r3.u32);
    *mode = {};

    // 720p: the console's standard HD mode and what Gears of War renders at.
    StoreBE<uint32_t>(&mode->displayWidth, 1280);
    StoreBE<uint32_t>(&mode->displayHeight, 720);
    StoreBE<uint32_t>(&mode->isInterlaced, 0);
    StoreBE<uint32_t>(&mode->isWidescreen, 1);
    StoreBE<uint32_t>(&mode->isHiDef, 1);
    // 60.0f, stored big-endian through its bit pattern.
    StoreBE<uint32_t>(&mode->refreshRate, 0x42700000u);
    StoreBE<uint32_t>(&mode->videoStandard, 1); // NTSC
    StoreBE<uint32_t>(&mode->unknown1c, 0x4A);
    StoreBE<uint32_t>(&mode->unknown20, 0x01);

    lucent::info("video", "XGetVideoMode -> 1280x720 progressive widescreen 60Hz");
}

// 1 == XC_LANGUAGE_ENGLISH.
void __imp__XGetLanguage(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 1;
}

// 0x010000 == HDTV via component. Chosen to match the 720p mode reported above;
// an AV pack implying SD would contradict it.
void __imp__XGetAVPack(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0x010000;
}

// 0x00FF == XC_GAME_REGION_NA.
void __imp__XGetGameRegion(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0x00FF;
}
