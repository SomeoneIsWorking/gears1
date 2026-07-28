// The object-manager imports that translate between handles and the guest
// pointers the dispatcher APIs use.
#include "import_stub.h"

#include <byteswap.h>
#include <lucent/log.h>

#include <algorithm>
#include <vector>

#include "guest_thread.h"
#include "kernel_objects.h"

// NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, POBJECT_TYPE ObjectType,
//                                    PVOID* ReturnedObject)
void __imp__ObReferenceObjectByHandle(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t returnedObjectPtr = ctx.r5.u32;

    const uint32_t guestObject = gears::GuestAddressForHandle(handle);
    if (guestObject == 0)
    {
        lucent::error("kernel", "ObReferenceObjectByHandle: unknown handle {:#x}", handle);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    if (returnedObjectPtr != 0)
        *reinterpret_cast<uint32_t*>(base + returnedObjectPtr) = ByteSwap(guestObject);

    lucent::debug("kernel", "ObReferenceObjectByHandle({:#x}) -> {:#x}", handle, guestObject);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__ObDereferenceObject(PPCContext& __restrict ctx, uint8_t*)
{
    // Objects are owned by shared_ptr on the host and outlive the guest's
    // references, so there is no count to decrement. Guest lifetime bugs will
    // therefore not reproduce here, which is a difference worth remembering
    // rather than a behaviour to rely on.
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS KeWaitForSingleObject(PVOID Object, WAIT_REASON, WAIT_MODE,
//                                BOOLEAN Alertable, PLARGE_INTEGER Timeout)
void __imp__KeWaitForSingleObject(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t objectAddress = ctx.r3.u32;
    const uint32_t timeoutPtr = ctx.r7.u32;

    // Not finding it is normal: the title may have initialised this dispatcher
    // object in its own memory without ever going through a handle.
    auto object = gears::BindGuestDispatcherObject(objectAddress);
    if (!object)
    {
        lucent::debug("wait", "KeWait -> object {:#x} unbindable", objectAddress);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    int64_t timeout = -1;
    if (timeoutPtr != 0)
    {
        const int64_t raw = int64_t(ByteSwap(*reinterpret_cast<uint64_t*>(base + timeoutPtr)));
        if (raw > 0)
            lucent::warn("kernel", "absolute wait timeout {} not supported, waiting forever", raw);
        else
            timeout = -raw;
    }

    // Handle-based waits are logged by handle in kernel_sync.cpp; this is the
    // pointer-based path titles use for dispatcher objects embedded in their
    // own structures (the D3D per-CPU interrupt events among them), so it needs
    // its own line or those waits are invisible.
    lucent::debug("wait", "[{}] KeWait -> object {:#x} timeout {}",
        gears::GuestThreadName(), objectAddress, timeout);
    const bool signalled = object->Wait(timeout);
    lucent::debug("wait", "[{}] KeWait <- object {:#x} {}", gears::GuestThreadName(),
        objectAddress, signalled ? "signalled" : "timed out");
    ctx.r3.u64 = signalled ? gears::kStatusSuccess : gears::kStatusTimeout;
}

// NTSTATUS KeWaitForMultipleObjects(ULONG Count, PVOID Objects[], WAIT_TYPE,
//                                   WAIT_REASON, WAIT_MODE, BOOLEAN Alertable,
//                                   PLARGE_INTEGER Timeout, PKWAIT_BLOCK)
//
// WaitType 0 is WaitAll, 1 is WaitAny. The title's audio callback uses WaitAny
// on two objects with no timeout (catalog #40), which is what made this the
// blocker for driving the audio pump.
//
// The wait itself is KernelObject::WaitMultiple, not a loop over
// KeWaitForSingleObject: a synchronisation event or semaphore is CONSUMED by
// whoever takes it, so testing objects one at a time can swallow a signal from
// an object the caller then abandons. The test and the consume have to be atomic
// across all of them.
void __imp__KeWaitForMultipleObjects(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t count = ctx.r3.u32;
    const uint32_t objectsPtr = ctx.r4.u32;
    const uint32_t waitType = ctx.r5.u32;
    const uint32_t timeoutPtr = ctx.r9.u32;
    const bool waitAll = waitType == 0;

    if (count == 0 || count > 64 || objectsPtr == 0)
    {
        lucent::warn("wait", "KeWaitForMultipleObjects: count {} objects {:#x}"
            " is not a wait this can serve", count, objectsPtr);
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    // The array holds POINTERS to dispatcher objects, in guest memory.
    std::vector<std::shared_ptr<gears::KernelObject>> owned;
    std::vector<gears::KernelObject*> objects;
    std::vector<uint32_t> addresses;
    owned.reserve(count);
    objects.reserve(count);
    addresses.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t address =
            ByteSwap(*reinterpret_cast<uint32_t*>(base + objectsPtr + i * 4));
        auto object = gears::BindGuestDispatcherObject(address);
        if (!object)
        {
            // Reported rather than skipped: a wait on an object we cannot bind
            // would block forever or return early, and both are silent lies.
            lucent::warn("wait", "KeWaitForMultipleObjects: object {} at {:#x}"
                " unbindable", i, address);
            ctx.r3.u64 = gears::kStatusInvalidHandle;
            return;
        }
        owned.push_back(object);
        objects.push_back(object.get());
        addresses.push_back(address);
    }

    int64_t timeout = -1;
    if (timeoutPtr != 0)
    {
        const int64_t raw =
            int64_t(ByteSwap(*reinterpret_cast<uint64_t*>(base + timeoutPtr)));
        if (raw > 0)
            lucent::warn("kernel", "absolute wait timeout {} not supported,"
                " waiting forever", raw);
        else
            timeout = -raw;
    }

    {
        lucent::Line line;
        line.add("[{}] KeWaitMultiple -> {} timeout {} on", gears::GuestThreadName(),
                 waitAll ? "WaitAll" : "WaitAny", timeout);
        for (uint32_t i = 0; i < count; ++i)
            line.add(" [{:#x} {}]", addresses[i], objects[i]->KindName());
        line.flush_debug("wait");
    }

    // An untimed wait is served as a loop of bounded waits so that a wait
    // NOTHING will ever satisfy can say so. Blocking forever inside one call is
    // indistinguishable in a log from never having reached the call at all --
    // which is exactly the ambiguity the audio callback presented (catalog #40).
    // The semantics are unchanged: WaitMultiple consumes nothing when it times
    // out, so re-testing is the same as having waited throughout.
    constexpr int64_t kStuckReport100ns = 5 * 10'000'000; // 5 s
    int index = -1;
    for (int64_t waited = 0;;)
    {
        const int64_t slice =
            timeout < 0 ? kStuckReport100ns : std::min(timeout - waited, kStuckReport100ns);
        index = gears::KernelObject::WaitMultiple(objects.data(), count, waitAll, slice);
        if (index >= 0)
            break;
        waited += slice;
        if (timeout >= 0 && waited >= timeout)
            break;
        // Reported once per waiter, at the point it becomes suspicious, and then
        // left alone -- a wait that is legitimately long should not spam.
        if (waited == kStuckReport100ns)
        {
            lucent::Line line;
            line.add("[{}] KeWaitMultiple has blocked {} s on {} of", gears::GuestThreadName(),
                     waited / 10'000'000, waitAll ? "all" : "any");
            for (uint32_t i = 0; i < count; ++i)
                line.add(" [{:#x} {}]", addresses[i], objects[i]->KindName());
            line.add("; nothing has signalled them");
            line.flush(lucent::Level::Warn, "wait");
        }
    }
    lucent::debug("wait", "KeWaitMultiple <- {}", index);
    // WaitAny returns STATUS_WAIT_0 + index, which is simply the index.
    ctx.r3.u64 = index < 0 ? gears::kStatusTimeout : uint64_t(index);
}

// NTSTATUS NtDuplicateObject(HANDLE Handle, PHANDLE NewHandle, DWORD Options)
//
// A second handle onto the same object. The host side is a shared_ptr, so both
// handles genuinely refer to one object and signalling through either is seen
// by waiters on the other -- which is the whole point of duplicating one.
void __imp__NtDuplicateObject(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t newHandlePtr = ctx.r4.u32;

    auto object = gears::Handles().Lookup(handle);
    if (!object)
    {
        lucent::error("kernel", "NtDuplicateObject: unknown handle {:#x}", handle);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    const uint32_t duplicate = gears::Handles().Insert(std::move(object));
    if (newHandlePtr != 0)
        *reinterpret_cast<uint32_t*>(base + newHandlePtr) = ByteSwap(duplicate);

    lucent::debug("kernel", "NtDuplicateObject({:#x}) -> {:#x}", handle, duplicate);
    ctx.r3.u64 = gears::kStatusSuccess;
}
