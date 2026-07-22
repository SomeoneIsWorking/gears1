// Event objects and waiting.
#include "import_stub.h"

#include <chrono>
#include <thread>
#include <vector>

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

    // Logged on both sides of the wait: a hang is only diagnosable if the
    // record shows which handles were entered and never left.
    lucent::debug("wait", "-> {:#x} (timeout {})", handle, timeout100ns);
    const bool signalled = object->Wait(timeout100ns);
    lucent::debug("wait", "<- {:#x} {}", handle, signalled ? "signalled" : "timed out");
    return signalled ? gears::kStatusSuccess : gears::kStatusTimeout;
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

// NTSTATUS NtCreateMutant(PHANDLE Handle, POBJECT_ATTRIBUTES Attributes,
//                         BOOLEAN InitialOwner)
void __imp__NtCreateMutant(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handlePtr = ctx.r3.u32;
    const bool initialOwner = ctx.r5.u32 != 0;

    auto object = std::make_shared<gears::KernelObject>(
        gears::KernelObject::Kind::Mutant, false);
    if (initialOwner)
        object->Wait(0); // takes ownership for the creating thread

    const uint32_t handle = gears::Handles().Insert(std::move(object));
    if (handlePtr != 0)
        *reinterpret_cast<uint32_t*>(base + handlePtr) = ByteSwap(handle);

    lucent::debug("kernel", "NtCreateMutant(initialOwner={}) -> {:#x}", initialOwner, handle);
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS NtReleaseMutant(HANDLE Handle, PLONG PreviousCount)
void __imp__NtReleaseMutant(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t handle = ctx.r3.u32;
    auto object = gears::Handles().Lookup(handle);
    if (object == nullptr)
    {
        lucent::warn("kernel", "NtReleaseMutant({:#x}): no such handle", handle);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    if (!object->ReleaseMutant())
    {
        lucent::warn("kernel", "NtReleaseMutant({:#x}): calling thread is not the owner", handle);
        ctx.r3.u64 = gears::kStatusMutantNotOwned;
        return;
    }

    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, PHANDLE Handles,
//     WAIT_TYPE WaitType, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
//     PLARGE_INTEGER Timeout)
// WaitType 0 waits for all objects, 1 for any; the any-wait returns
// STATUS_WAIT_0 + the index of the object that satisfied it.
void __imp__NtWaitForMultipleObjectsEx(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t count = ctx.r3.u32;
    const uint32_t handlesPtr = ctx.r4.u32;
    const uint32_t waitType = ctx.r5.u32;
    const int64_t timeout100ns = ReadTimeout(base, ctx.r8.u32);

    if (count == 0 || handlesPtr == 0)
    {
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    std::vector<uint32_t> handles(count);
    std::vector<std::shared_ptr<gears::KernelObject>> objects(count);
    for (uint32_t i = 0; i < count; i++)
    {
        handles[i] = ByteSwap(*reinterpret_cast<uint32_t*>(base + handlesPtr + i * 4));
        objects[i] = gears::Handles().Lookup(handles[i]);
        if (objects[i] == nullptr)
        {
            lucent::error("wait", "NtWaitForMultipleObjectsEx: unknown handle {:#x}", handles[i]);
            ctx.r3.u64 = gears::kStatusInvalidHandle;
            return;
        }
    }

    if (waitType == 0)
    {
        // Wait-all: acquired one after another. Not atomic the way the real
        // dispatcher is, but the objects are acquired in a stable order so two
        // all-waiters cannot deadlock against each other on the same set.
        for (uint32_t i = 0; i < count; i++)
        {
            if (!objects[i]->Wait(timeout100ns))
            {
                ctx.r3.u64 = gears::kStatusTimeout;
                return;
            }
        }
        ctx.r3.u64 = gears::kStatusSuccess;
        return;
    }

    // Wait-any. The objects have independent signal states, so this polls:
    // each pass try-acquires every object with a zero timeout, then sleeps
    // briefly. Millisecond-scale latency, which is the granularity the
    // console's own scheduler quantum gave titles anyway.
    const auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            if (objects[i]->Wait(0))
            {
                ctx.r3.u64 = i; // STATUS_WAIT_0 + i
                return;
            }
        }

        if (timeout100ns >= 0)
        {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
                >= timeout100ns * 100)
            {
                ctx.r3.u64 = gears::kStatusTimeout;
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void __imp__NtSetEvent(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("wait", "NtSetEvent({:#x})", ctx.r3.u32);
    if (auto object = gears::Handles().Lookup(ctx.r3.u32))
        object->Set();
    else
        lucent::warn("wait", "NtSetEvent on unknown handle {:#x}", ctx.r3.u32);
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
    lucent::debug("wait", "NtPulseEvent({:#x})", ctx.r3.u32);
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

// File handles live in their own table, so closing has to try both.
bool CloseGuestFile(uint32_t handle);

void __imp__NtClose(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t handle = ctx.r3.u32;
    const bool closed = gears::Handles().Close(handle) || CloseGuestFile(handle);
    ctx.r3.u64 = closed ? gears::kStatusSuccess : gears::kStatusInvalidHandle;
}
