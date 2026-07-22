#include "xam_overlapped.h"

#include <lucent/log.h>

#include <byteswap.h>

#include "kernel_objects.h"

namespace gears
{

namespace
{
constexpr uint32_t kInternal = 0x00;
constexpr uint32_t kInternalHigh = 0x04;
constexpr uint32_t kEventHandle = 0x0C;
constexpr uint32_t kCompletionRoutine = 0x10;
constexpr uint32_t kExtendedError = 0x18;

void Store32(uint8_t* base, uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(base + address) = ByteSwap(value);
}
} // namespace

void CompleteOverlapped(uint8_t* base, uint32_t overlapped, uint32_t result,
    uint32_t bytesTransferred)
{
    if (overlapped == 0)
        return;

    Store32(base, overlapped + kInternal, result);
    Store32(base, overlapped + kInternalHigh, bytesTransferred);
    Store32(base, overlapped + kExtendedError, result);

    const uint32_t completionRoutine =
        ByteSwap(*reinterpret_cast<uint32_t*>(base + overlapped + kCompletionRoutine));
    if (completionRoutine != 0)
    {
        // Guest callbacks are not dispatched from here. Reported rather than
        // dropped: a caller relying on the routine would otherwise never run
        // and nothing would say why.
        lucent::warn("xam", "overlapped {:#x} wanted completion routine {:#x}, not dispatched",
            overlapped, completionRoutine);
    }

    // Signalled last, so a waiter that wakes immediately sees the result
    // fields already written rather than racing them.
    const uint32_t eventHandle =
        ByteSwap(*reinterpret_cast<uint32_t*>(base + overlapped + kEventHandle));
    if (eventHandle != 0)
    {
        if (auto object = Handles().Lookup(eventHandle))
            object->Set();
        else
            lucent::warn("xam", "overlapped {:#x} names unknown event {:#x}", overlapped, eventHandle);
    }

    lucent::debug("xam", "completed overlapped {:#x} with {:#x}", overlapped, result);
}

} // namespace gears
