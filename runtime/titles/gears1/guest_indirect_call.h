#pragma once

#include <cstdint>

struct PPCContext;

namespace gears::titles::gears1
{

inline void CallGuestIndirect(PPCContext &ctx, std::uint8_t *base, std::uint32_t target);

} // namespace gears::titles::gears1

// One exact title module is linked into this executable. Keep its diagnostic
// selector in the forced title header so the shared checked-call owner has no
// guest addresses and the hot path remains one inlined comparison.
#define PPC_CALL_INDIRECT_FUNC(x)                                                                  \
    gears::titles::gears1::CallGuestIndirect(ctx, base, std::uint32_t(x))

#include "../../guest_indirect_call.h"

namespace gears::titles::gears1
{

inline constexpr std::uint32_t kStreamingCallReturn = 0x823EDB50;

[[nodiscard]] constexpr bool ShouldObserveStreamingObject(std::uint32_t returnAddress)
{
    return returnAddress == kStreamingCallReturn;
}

void NoteStreamingObject(PPCContext &ctx, std::uint8_t *base);

inline void CallGuestIndirect(PPCContext &ctx, std::uint8_t *base, std::uint32_t target)
{
    if (ShouldObserveStreamingObject(std::uint32_t(ctx.lr))) [[unlikely]]
        NoteStreamingObject(ctx, base);
    gears::CallGuestIndirect(ctx, base, target);
}

} // namespace gears::titles::gears1
