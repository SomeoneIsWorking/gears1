// A call stack for guest code.
//
// Recompiled guest functions are ordinary C++ functions, so a host backtrace
// shows `__imp__sub_X` frames only where the recompiler happened not to inline
// them -- and says nothing at all about a chain reached through the guest's own
// function-pointer tables. The guest's stack, on the other hand, is intact and
// laid out by the Xbox 360 ABI, so it can simply be walked.
//
// This replaces the probe-per-level loop: chasing a value up an allocator chain
// one strong `sub_X` override at a time costs a full run per level, and each run
// answers exactly one question. A backtrace answers all of them at once.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gears
{

// Return addresses, innermost first. `lr` is the current link register, which
// is the innermost frame and is not yet on the stack.
std::vector<uint32_t> GuestBacktrace(uint32_t stackPointer, uint32_t lr,
                                     size_t maxFrames = 24);

// The same, formatted as "0x82215898 <- 0x822158e4 <- ..." for a log line.
std::string FormatGuestBacktrace(uint32_t stackPointer, uint32_t lr,
                                 size_t maxFrames = 24);

} // namespace gears
