// Dispatcher objects that live in guest memory.
//
// Titles embed KEVENTs and KSEMAPHOREs inside their own structures and
// initialise them in place, so these never pass through a handle. Each one is
// bound to a host object on first use, seeded from the guest's own dispatch
// header rather than from an assumption about its type or state.
#include "import_stub.h"

#include <byteswap.h>
#include <lucent/log.h>

#include "kernel_objects.h"

namespace
{

void WriteDispatchHeader(uint8_t* base, uint32_t address, uint32_t type, int32_t signalState)
{
    *reinterpret_cast<uint32_t*>(base + address) = ByteSwap(type << 24);
    *reinterpret_cast<uint32_t*>(base + address + 4) = ByteSwap(uint32_t(signalState));
    // The wait list is a guest-side doubly linked list that points at itself
    // when empty. Waiters are tracked host-side, but the guest reads these.
    *reinterpret_cast<uint32_t*>(base + address + 8) = ByteSwap(address + 8);
    *reinterpret_cast<uint32_t*>(base + address + 12) = ByteSwap(address + 8);
}

std::shared_ptr<gears::KernelObject> Bind(uint32_t address, const char* who)
{
    auto object = gears::BindGuestDispatcherObject(address);
    if (!object)
        lucent::error("kernel", "{}: cannot bind dispatcher object at {:#x}", who, address);
    return object;
}

} // namespace

// VOID KeInitializeSemaphore(PKSEMAPHORE Semaphore, LONG Count, LONG Limit)
void __imp__KeInitializeSemaphore(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t address = ctx.r3.u32;
    const int32_t count = int32_t(ctx.r4.u32);
    const int32_t limit = int32_t(ctx.r5.u32);

    WriteDispatchHeader(base, address, 5 /* SemaphoreObject */, count);
    *reinterpret_cast<uint32_t*>(base + address + 16) = ByteSwap(uint32_t(limit));

    gears::RegisterGuestObject(address, std::make_shared<gears::KernelObject>(count, limit));
    lucent::debug("kernel", "KeInitializeSemaphore({:#x}, count={}, limit={})", address, count, limit);
}

// LONG KeReleaseSemaphore(PKSEMAPHORE, LONG Increment, LONG Adjustment, BOOLEAN Wait)
void __imp__KeReleaseSemaphore(PPCContext& __restrict ctx, uint8_t* base)
{
    auto object = Bind(ctx.r3.u32, "KeReleaseSemaphore");
    if (!object)
    {
        ctx.r3.u64 = 0;
        return;
    }

    const int32_t previous = object->Release(int32_t(ctx.r5.u32));
    *reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + 4) = ByteSwap(uint32_t(previous + int32_t(ctx.r5.u32)));
    ctx.r3.u64 = uint32_t(previous);
}

// LONG KeSetEvent(PRKEVENT Event, LONG Increment, BOOLEAN Wait)
void __imp__KeSetEvent(PPCContext& __restrict ctx, uint8_t* base)
{
    lucent::debug("wait", "KeSetEvent(object {:#x})", ctx.r3.u32);
    auto object = Bind(ctx.r3.u32, "KeSetEvent");
    if (object)
    {
        object->Set();
        *reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + 4) = ByteSwap(uint32_t(1));
    }
    ctx.r3.u64 = 0;
}

// LONG KeResetEvent(PRKEVENT Event)
void __imp__KeResetEvent(PPCContext& __restrict ctx, uint8_t* base)
{
    auto object = Bind(ctx.r3.u32, "KeResetEvent");
    if (object)
    {
        object->Clear();
        *reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + 4) = 0;
    }
    ctx.r3.u64 = 0;
}
