// The object-manager imports that translate between handles and the guest
// pointers the dispatcher APIs use.
#include "import_stub.h"

#include <byteswap.h>
#include <lucent/log.h>

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

    auto object = gears::LookupByGuestAddress(objectAddress);
    if (!object)
    {
        lucent::error("kernel", "KeWaitForSingleObject: unknown object {:#x}", objectAddress);
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

    ctx.r3.u64 = object->Wait(timeout) ? gears::kStatusSuccess : gears::kStatusTimeout;
}
