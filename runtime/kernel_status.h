#pragma once

#include <cstdint>

namespace gears
{

// NTSTATUS values the kernel imports return in r3.
constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusTimeout = 0x00000102;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusNoMemory = 0xC0000017;
constexpr uint32_t kStatusNotFound = 0xC0000225;

// Xam returns Win32-style error codes rather than NTSTATUS.
constexpr uint32_t kErrorSuccess = 0x00000000;
constexpr uint32_t kErrorNotFound = 0x00000490;

} // namespace gears
