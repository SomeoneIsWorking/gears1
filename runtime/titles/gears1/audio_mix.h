#pragma once

struct PPCContext;

namespace gears::titles::gears1
{

// Independently authored implementation of the exact-revision 256-sample
// mixer contract at 0x825F2D40. Future dispatch must enter through x360port;
// this declaration does not provide a guest executor.
void ApplyNativeAudioMix(PPCContext &ctx, unsigned char *base);

} // namespace gears::titles::gears1
