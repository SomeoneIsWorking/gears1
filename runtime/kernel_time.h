#pragma once

#include <cstdint>

#include "guest_memory.h"

namespace gears
{

// KeTimeStampBundle is an exported kernel VARIABLE the kernel updates
// continuously; titles read wall-clock state from it without a syscall:
//   +0x00  u64  interrupt time, 100 ns units, monotonic
//   +0x08  u64  system time, 100 ns units since 1601-01-01
//   +0x10  u32  tick count, milliseconds, monotonic (the GetTickCount source)
// Starts a host thread that refreshes the bundle at millisecond granularity.
// Values agree with KeQuerySystemTime / the timebase by construction.
void StartKeTimeStampBundle(GuestMemory& memory, uint32_t guestAddress);

} // namespace gears
