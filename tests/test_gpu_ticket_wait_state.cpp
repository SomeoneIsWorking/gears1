#include "gpu_ticket_wait_state.h"
#include "guest_state_memory.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main()
{
    constexpr std::uint32_t kState = 0x100;
    constexpr std::uint32_t kDevice = 0x400;
    constexpr std::uint32_t kProgress = 0x200;
    constexpr std::uint32_t kThreadRegister = 0x300;
    constexpr std::uint32_t kThread = 0x500;

    std::vector<std::uint8_t> guest(0x4000);
    gears::titles::gears1::GuestStateMemory memory(guest.data());
    memory.Write32(kState, kDevice);
    memory.Write32(kState + 4, 3);
    memory.Write32(kState + 8, 17);
    memory.Write32(kState + 12, 1'000);
    memory.Write32(kState + 16, 900);
    memory.Write32(kState + 20, 0x1234'5678);
    memory.Write32(kDevice + 10'768, kProgress);
    memory.Write32(kDevice + 10'760, 7);
    memory.Write32(kDevice + 10'864, 1);
    memory.Write32(kThreadRegister + 256, kThread);
    memory.Write32(kThread + 332, 7);
    memory.Write32(kThread + 88, 1'250);

    const auto state =
        gears::titles::gears1::ReadGpuTicketWaitState(memory, kState, kThreadRegister);
    assert(state.has_value());
    assert(state->operation == 3);
    assert(state->progressAddress == kProgress);
    assert(state->lastProgress == 17);
    assert(state->lastProgressTick == 1'000);
    assert(state->initialTick == 900);
    assert(state->initialTimeBase == 0x1234'5678);
    assert(state->currentTick == 1'250);
    assert(!state->owningThreadExempt);
    assert(state->refreshDeadline);

    memory.Write8(kDevice + 10'809, 0x2);
    const auto ownerState =
        gears::titles::gears1::ReadGpuTicketWaitState(memory, kState, kThreadRegister);
    assert(ownerState.has_value());
    assert(ownerState->owningThreadExempt);

    memory.Write32(kState, 0);
    assert(!gears::titles::gears1::ReadGpuTicketWaitState(memory, kState, kThreadRegister)
                .has_value());

    return 0;
}
