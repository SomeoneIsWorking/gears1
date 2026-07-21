// Event objects and waiting.
#include "import_stub.h"

#include <byteswap.h>
#include <lucent/log.h>

#include "kernel_objects.h"

namespace
{
// The guest passes an optional pointer to a 64-bit timeout in 100 ns units.
// Negative means relative, positive means an absolute time; a null pointer
// means wait forever.
int64_t ReadTimeout(uint8_t* base, uint32_t timeoutPtr)
{
    if (timeoutPtr == 0)
        return -1;

    const int64_t raw = int64_t(ByteSwap(*reinterpret_cast<uint64_t*>(base + timeoutPtr)));
    // Only relative timeouts are handled; an absolute one would need the
    // console's clock epoch, and treating it as relative would hang for years.
    if (raw > 0)
    {
        lucent::warn("kernel", "absolute wait timeout {} not supported, waiting forever", raw);
        return -1;
    }
    return -raw;
}

uint32_t WaitOn(uint32_t handle, int64_t timeout100ns)
{
    auto object = gears::Handles().Lookup(handle);
    if (!object)
    {
        lucent::error("kernel", "wait on unknown handle {:#x}", handle);
        return gears::kStatusInvalidHandle;
    }

    return object->Wait(timeout100ns) ? gears::kStatusSuccess : gears::kStatusTimeout;
}
} // namespace

// NTSTATUS NtCreateEvent(PHANDLE Handle, POBJECT_ATTRIBUTES Attributes,
//                        EVENT_TYPE Type, BOOLEAN InitialState)
// Type 0 is a notification event, 1 a synchronisation event.
void __imp__NtCreateEvent(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handlePtr = ctx.r3.u32;
    const uint32_t type = ctx.r5.u32;
    const bool initialState = ctx.r6.u32 != 0;

    const auto kind = type == 0 ? gears::KernelObject::Kind::NotificationEvent
                                : gears::KernelObject::Kind::SynchronizationEvent;

    const uint32_t handle = gears::Handles().Insert(
        std::make_shared<gears::KernelObject>(kind, initialState));

    if (handlePtr != 0)
        *reinterpret_cast<uint32_t*>(base + handlePtr) = ByteSwap(handle);

    lucent::debug("kernel", "NtCreateEvent(type={}, initial={}) -> {:#x}",
        type, initialState, handle);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__NtSetEvent(PPCContext& __restrict ctx, uint8_t*)
{
    if (auto object = gears::Handles().Lookup(ctx.r3.u32))
        object->Set();
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__NtClearEvent(PPCContext& __restrict ctx, uint8_t*)
{
    if (auto object = gears::Handles().Lookup(ctx.r3.u32))
        object->Clear();
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__NtPulseEvent(PPCContext& __restrict ctx, uint8_t*)
{
    if (auto object = gears::Handles().Lookup(ctx.r3.u32))
        object->Pulse();
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS NtWaitForSingleObjectEx(HANDLE Handle, WAIT_MODE WaitMode,
//                                  BOOLEAN Alertable, PLARGE_INTEGER Timeout)
void __imp__NtWaitForSingleObjectEx(PPCContext& __restrict ctx, uint8_t* base)
{
    ctx.r3.u64 = WaitOn(ctx.r3.u32, ReadTimeout(base, ctx.r6.u32));
}

void __imp__NtClose(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::Handles().Close(ctx.r3.u32)
        ? gears::kStatusSuccess
        : gears::kStatusInvalidHandle;
}
