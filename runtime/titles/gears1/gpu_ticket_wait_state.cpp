#include "gpu_ticket_wait_state.h"

#include "guest_state_memory.h"

namespace gears::titles::gears1
{

namespace
{

constexpr std::uint32_t kAdaptiveWaitOperationOffset = 4;
constexpr std::uint32_t kAdaptiveWaitLastProgressOffset = 8;
constexpr std::uint32_t kAdaptiveWaitLastProgressTickOffset = 12;
constexpr std::uint32_t kAdaptiveWaitInitialTickOffset = 16;
constexpr std::uint32_t kAdaptiveWaitInitialTimeBaseOffset = 20;
constexpr std::uint32_t kDeviceProgressBlockOffset = 10'768;
constexpr std::uint32_t kDeviceWaitFlagsOffset = 10'809;
constexpr std::uint32_t kDeviceOwnerProcessorOffset = 10'760;
constexpr std::uint32_t kDeviceRefreshDeadlineOffset = 10'864;
constexpr std::uint8_t kOwningThreadExemption = 0x2;
constexpr std::uint32_t kCurrentThreadPointerOffset = 256;
constexpr std::uint32_t kThreadTickCountOffset = 88;
constexpr std::uint32_t kThreadProcessorOffset = 332;

} // namespace

std::optional<GpuTicketWaitState> ReadGpuTicketWaitState(const GuestStateMemory &memory,
                                                         std::uint32_t state,
                                                         std::uint32_t threadRegister)
{
    if (state == 0 || threadRegister == 0)
        return std::nullopt;

    const std::uint32_t device = memory.Read32(state);
    if (device == 0)
        return std::nullopt;
    const std::uint32_t progressAddress = memory.Read32(device + kDeviceProgressBlockOffset);
    if (progressAddress == 0)
        return std::nullopt;
    const std::uint32_t thread = memory.Read32(threadRegister + kCurrentThreadPointerOffset);
    if (thread == 0)
        return std::nullopt;

    const bool refreshDeadline = memory.Read32(device + kDeviceOwnerProcessorOffset) ==
                                     memory.Read32(thread + kThreadProcessorOffset) &&
                                 memory.Read32(device + kDeviceRefreshDeadlineOffset) != 0;

    return GpuTicketWaitState{
        .operation = memory.Read32(state + kAdaptiveWaitOperationOffset),
        .progressAddress = progressAddress,
        .lastProgress = memory.Read32(state + kAdaptiveWaitLastProgressOffset),
        .lastProgressTick = memory.Read32(state + kAdaptiveWaitLastProgressTickOffset),
        .initialTick = memory.Read32(state + kAdaptiveWaitInitialTickOffset),
        .initialTimeBase = memory.Read32(state + kAdaptiveWaitInitialTimeBaseOffset),
        .currentTick = memory.Read32(thread + kThreadTickCountOffset),
        .owningThreadExempt =
            (memory.Read8(device + kDeviceWaitFlagsOffset) & kOwningThreadExemption) != 0,
        .refreshDeadline = refreshDeadline,
    };
}

} // namespace gears::titles::gears1
