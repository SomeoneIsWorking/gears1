// Reading arguments the guest passed on the STACK rather than in a register.
//
// The PowerPC ABI this title is built with passes the first eight integer
// arguments in r3..r10 and everything after that in the caller's parameter save
// area, which begins at r1+0x14 with eight-byte slots. So the ninth argument
// lives at r1 + 0x14 + 8*8 = r1+84, and an import that reads it out of a
// register reads something else entirely.
//
// THAT IS NOT HYPOTHETICAL. XamContentCreateEx takes nine parameters:
//
//   (user, root, data, flags, disposition, licenseMask, cacheSize,
//    contentSize, overlapped)
//
// Our implementation documented a SEVEN-parameter signature and read the
// overlapped block out of r9 -- which is cacheSize, and which the title's own
// wrapper at 0x82611900 explicitly zeroes before spilling the real overlapped to
// the stack:
//
//   stw r9,84(r1)      ; the caller's overlapped -> the stack
//   li  r9,0           ; and r9 is now cacheSize = 0
//
// so we saw overlapped == 0, completed synchronously, and returned success. The
// title's checkpoint loader REQUIRES ERROR_IO_PENDING to continue; given success
// it concluded there was nothing to load and reported success having loaded
// nothing. Everything in issue #45 follows from that one wrong register.
#pragma once

#include <cstdint>

namespace gears
{

// Byte offset of the Nth argument (zero-based) within the caller's frame. Only
// meaningful for index >= 8; the first eight are in registers.
constexpr uint32_t kParameterSaveArea = 0x14;
constexpr uint32_t kParameterSlotSize = 8;

constexpr uint32_t GuestStackArgumentOffset(uint32_t index)
{
    return kParameterSaveArea + kParameterSlotSize * index;
}

// Reads a 32-bit argument the guest passed on the stack. `index` is zero-based
// across the whole argument list, so the ninth argument is index 8.
//
// The value sits at the START of its slot: the compiler stores 32-bit arguments
// there with a plain `stw`, which is what the title's own wrapper does.
uint32_t GuestStackArgument32(const uint8_t* base, uint32_t stackPointer,
                             uint32_t index);

} // namespace gears
