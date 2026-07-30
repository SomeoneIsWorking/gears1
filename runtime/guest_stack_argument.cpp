#include "guest_stack_argument.h"

namespace gears
{

uint32_t GuestStackArgument32(const uint8_t* base, uint32_t stackPointer,
                             uint32_t index)
{
    const uint32_t address = stackPointer + GuestStackArgumentOffset(index);
    const uint8_t* p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

} // namespace gears
