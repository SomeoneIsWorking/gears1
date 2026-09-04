// A call stack for guest code.
//
// The guest stack is laid out by the Xbox 360 ABI independently of which Xenia
// dynarec host architecture executes it, so it can be walked from captured
// guest registers.
//
// This preserves an independently useful diagnostic while the old generated
// host-function backtrace path is gone.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gears
{

// Return addresses, innermost first. `lr` is the current link register, which
// is the innermost frame and is not yet on the stack.
std::vector<uint32_t> GuestBacktrace(uint32_t stackPointer, uint32_t lr, size_t maxFrames = 24);

// The same, formatted as "0x82215898 <- 0x822158e4 <- ..." for a log line.
std::string FormatGuestBacktrace(uint32_t stackPointer, uint32_t lr, size_t maxFrames = 24);

} // namespace gears
