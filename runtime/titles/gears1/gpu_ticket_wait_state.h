#pragma once

#include <cstdint>
#include <optional>

namespace gears::titles::gears1
{

class GuestStateMemory;

struct GpuTicketWaitState
{
    std::uint32_t operation = 0;
    std::uint32_t progressAddress = 0;
    std::uint32_t lastProgress = 0;
    std::uint32_t lastProgressTick = 0;
    std::uint32_t initialTick = 0;
    std::uint32_t initialTimeBase = 0;
    std::uint32_t currentTick = 0;
    bool owningThreadExempt = false;
    bool refreshDeadline = false;
};

[[nodiscard]] std::optional<GpuTicketWaitState>
ReadGpuTicketWaitState(const GuestStateMemory &memory, std::uint32_t state,
                       std::uint32_t threadRegister);

} // namespace gears::titles::gears1
