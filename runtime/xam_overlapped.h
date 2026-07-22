#pragma once

#include <cstdint>

namespace gears
{

// XOVERLAPPED, the console's async request block:
//   +0x00 Internal            result status
//   +0x04 InternalHigh        bytes transferred
//   +0x08 InternalContext
//   +0x0C hEvent              signalled on completion
//   +0x10 pCompletionRoutine
//   +0x14 dwCompletionContext
//   +0x18 dwExtendedError
//
// Refusing an async request is not the same as failing it. A caller hands over
// an XOVERLAPPED and then waits on it, so a request that is declined but never
// completed leaves that caller waiting forever -- which is exactly how the
// title stalled after startup. Declining is fine; declining silently is not.
//
// Completes the request with `result`, signalling the caller's event if it
// supplied one. Safe to call with a null pointer: a synchronous caller passes
// none, and then there is nothing to complete.
void CompleteOverlapped(uint8_t* base, uint32_t overlapped, uint32_t result,
    uint32_t bytesTransferred = 0);

} // namespace gears
