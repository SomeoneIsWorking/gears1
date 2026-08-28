#pragma once

struct PPCContext;

namespace gears::titles::gears1
{

// Native translation of the exact-revision 256-sample mixer kernel at
// 0x825F2D40. The generated body remains the compatibility and audit arm.
void ApplyNativeAudioMix(PPCContext &ctx, unsigned char *base);

} // namespace gears::titles::gears1
