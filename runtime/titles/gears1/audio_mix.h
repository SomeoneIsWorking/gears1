#pragma once

#include <cstdint>
#include <span>

#include <x360port/guest_call.hpp>

namespace gears::titles::gears1
{

inline constexpr x360port::GuestAddress kAudioMixAddress = 0x825F2D40U;

// Independently authored implementation of the exact-revision 256-sample
// mixer contract at guest address 0x825F2D40. Dispatch enters through x360port;
// this declaration does not provide a guest executor.
x360port::ExecutionResult ApplyNativeAudioMix(x360port::GuestCallContext &call,
                                              x360port::GuestAddress address,
                                              std::span<const std::uint64_t> arguments,
                                              void *context) noexcept;

} // namespace gears::titles::gears1
