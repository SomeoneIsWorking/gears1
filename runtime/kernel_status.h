#pragma once

#include <cstdint>

namespace gears
{

// NTSTATUS values the kernel imports return in r3.
constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
constexpr uint32_t kStatusTimeout = 0x00000102;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusNoMemory = 0xC0000017;
constexpr uint32_t kStatusMutantNotOwned = 0xC0000046;
constexpr uint32_t kStatusNotFound = 0xC0000225;

// Xam returns Win32-style error codes rather than NTSTATUS.
constexpr uint32_t kErrorSuccess = 0x00000000;
constexpr uint32_t kErrorNotFound = 0x00000490;
constexpr uint32_t kErrorPathNotFound = 0x00000003;
constexpr uint32_t kErrorAccessDenied = 0x00000005;
constexpr uint32_t kErrorInvalidParameter = 0x00000057;
constexpr uint32_t kErrorAlreadyExists = 0x000000B7;
constexpr uint32_t kErrorInsufficientBuffer = 0x0000007A;
constexpr uint32_t kErrorDeviceNotConnected = 0x0000048F;
constexpr uint32_t kErrorNoSuchUser = 0x00000525;
constexpr uint32_t kErrorNoMoreFiles = 0x00000012;
constexpr uint32_t kErrorInvalidHandle = 0x00000006;

// An asynchronous request that was accepted. A caller that supplied an
// XOVERLAPPED expects this rather than a result: the outcome reaches it through
// the overlapped, which may already be complete by the time this returns.
constexpr uint32_t kErrorIoPending = 0x000003E5;

} // namespace gears
