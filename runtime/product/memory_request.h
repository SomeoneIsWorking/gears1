#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gears::product
{

// The most guest memory one control-channel read returns.
inline constexpr std::uint32_t kMaxMemoryReadBytes = 4096;

// One guest-memory read the control channel was asked for.
struct MemoryRequest
{
    std::uint32_t address = 0;
    std::uint32_t length = 0;
};

// A read from URL-encoded fields: address=0x82BED138 (hexadecimal, 0x
// optional) and length in 1..kMaxMemoryReadBytes. Returns false with a reason
// naming the field when either is absent, malformed, out of range, or the
// range passes the top of the 32-bit guest address space, or on any other
// field.
[[nodiscard]] bool ParseMemoryRequest(std::string_view encoded, MemoryRequest &request,
                                      std::string &error);

} // namespace gears::product
