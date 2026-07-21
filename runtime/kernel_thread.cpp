// Guest thread creation.
//
// Every recompiled function takes its PPCContext by parameter, so a guest
// thread is just a host thread with its own context and its own thread block.
// The memory barriers the recompiler emits are what make this safe; they were
// implemented before this file existed, deliberately.
#include "import_stub.h"

#include <atomic>
#include <memory>
#include <thread>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_memory.h"
#include "guest_thread.h"
#include "kernel_objects.h"

namespace
{

constexpr uint32_t kCreateSuspended = 0x00000001;

std::atomic<uint32_t> g_nextThreadId{1};

struct GuestThreadStart
{
    gears::GuestThreadBlock block;
    uint32_t startupRoutine; // XAPI thread startup shim, may be 0
    uint32_t startAddress;
    uint32_t startContext;
    uint32_t threadId;
    std::shared_ptr<gears::KernelObject> exited;
    std::shared_ptr<gears::KernelObject> resumed;
};

void GuestThreadMain(std::shared_ptr<GuestThreadStart> start)
{
    uint8_t* base = gears::Memory().Base();

    // A suspended thread must not run guest code until it is resumed.
    if (start->resumed)
        start->resumed->Wait(-1);

    PPCContext ctx{};
    ctx.r13.u32 = start->block.pcrAddress;
    ctx.r1.u32 = start->block.stackBase - 0x100;
    ctx.fpscr.loadFromHost();

    lucent::info("thread", "guest thread {} entering {:#x} (context {:#x})",
        start->threadId, start->startAddress, start->startContext);

    // The XAPI startup shim, when present, is what the console calls; it takes
    // the real entry point and its argument and handles thread teardown.
    if (start->startupRoutine != 0)
    {
        ctx.r3.u32 = start->startAddress;
        ctx.r4.u32 = start->startContext;
        (PPC_LOOKUP_FUNC(base, start->startupRoutine))(ctx, base);
    }
    else
    {
        ctx.r3.u32 = start->startContext;
        (PPC_LOOKUP_FUNC(base, start->startAddress))(ctx, base);
    }

    lucent::info("thread", "guest thread {} exited", start->threadId);
    start->exited->Set();
}

} // namespace

// NTSTATUS ExCreateThread(PHANDLE Handle, ULONG StackSize, PULONG ThreadId,
//                         PVOID XapiThreadStartup, PVOID StartAddress,
//                         PVOID StartContext, ULONG CreationFlags)
void __imp__ExCreateThread(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handlePtr = ctx.r3.u32;
    uint32_t stackSize = ctx.r4.u32;
    const uint32_t threadIdPtr = ctx.r5.u32;
    const uint32_t startupRoutine = ctx.r6.u32;
    const uint32_t startAddress = ctx.r7.u32;
    const uint32_t startContext = ctx.r8.u32;
    const uint32_t creationFlags = ctx.r9.u32;

    if (stackSize == 0)
        stackSize = 0x10000;

    auto start = std::make_shared<GuestThreadStart>();
    if (!gears::CreateGuestThreadBlock(gears::Memory(), stackSize, start->block))
    {
        ctx.r3.u64 = gears::kStatusNoMemory;
        return;
    }

    start->startupRoutine = startupRoutine;
    start->startAddress = startAddress;
    start->startContext = startContext;
    start->threadId = g_nextThreadId.fetch_add(1);
    start->exited = std::make_shared<gears::KernelObject>(
        gears::KernelObject::Kind::NotificationEvent, false);

    if ((creationFlags & kCreateSuspended) != 0)
    {
        start->resumed = std::make_shared<gears::KernelObject>(
            gears::KernelObject::Kind::NotificationEvent, false);
    }

    // The handle waits on thread exit, which is what the guest joins against.
    const uint32_t handle = gears::Handles().Insert(start->exited);
    gears::RegisterThreadResume(handle, start->resumed);

    if (handlePtr != 0)
        *reinterpret_cast<uint32_t*>(base + handlePtr) = ByteSwap(handle);
    if (threadIdPtr != 0)
        *reinterpret_cast<uint32_t*>(base + threadIdPtr) = ByteSwap(start->threadId);

    lucent::info("thread", "ExCreateThread -> handle {:#x} id {} entry {:#x} stack {:#x}{}",
        handle, start->threadId, startAddress, stackSize,
        start->resumed ? " (suspended)" : "");

    std::thread(GuestThreadMain, start).detach();
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__NtResumeThread(PPCContext& __restrict ctx, uint8_t*)
{
    gears::ResumeThread(ctx.r3.u32);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__KeResumeThread(PPCContext& __restrict ctx, uint8_t*)
{
    gears::ResumeThread(ctx.r3.u32);
    ctx.r3.u64 = gears::kStatusSuccess;
}
