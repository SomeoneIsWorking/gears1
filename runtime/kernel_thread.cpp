// Guest thread creation.
//
// Every recompiled function takes its PPCContext by parameter, so a guest
// thread is just a host thread with its own context and its own thread block.
// The memory barriers the recompiler emits are what make this safe; they were
// implemented before this file existed, deliberately.
#include "import_stub.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <thread>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_memory.h"
#include "guest_thread.h"
#include "pump_probe.h"
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
    uint8_t processor;
    std::shared_ptr<gears::KernelObject> exited;
    std::shared_ptr<gears::KernelObject> resumed;
};

// Threads by handle, so an API that names a thread by the object pointer the
// title holds can get back to the block the runtime built for it.
std::mutex g_threadsMutex;
std::unordered_map<uint32_t, std::shared_ptr<GuestThreadStart>> g_threadsByHandle;

// Thrown by ExTerminateThread to unwind the calling guest thread back to
// GuestThreadMain. The recompiled functions are plain C++ with no cleanup of
// their own, so the unwind is safe; only the runtime's own frames run
// destructors on the way out.
struct GuestThreadExit
{
    uint32_t exitCode;
};

thread_local GuestThreadStart* t_currentThread = nullptr;

void GuestThreadMain(std::shared_ptr<GuestThreadStart> start)
{
    uint8_t* base = gears::Memory().Base();

    // A suspended thread must not run guest code until it is resumed.
    if (start->resumed)
        start->resumed->Wait(-1);

    t_currentThread = start.get();
    gears::SetGuestThreadName("guest-" + std::to_string(start->threadId));
    // So threads this one creates without a processor of their own inherit it,
    // which is what the console does.
    gears::SetCurrentGuestProcessor(start->processor);

    PPCContext ctx{};
    ctx.r13.u32 = start->block.pcrAddress;
    ctx.r1.u32 = start->block.stackBase - 0x100;
    ctx.fpscr.loadFromHost();

    lucent::info("thread", "guest thread {} entering {:#x} (context {:#x})",
        start->threadId, start->startAddress, start->startContext);

    try
    {
        // The XAPI startup shim, when present, is what the console calls; it
        // takes the real entry point and its argument and handles teardown.
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
    }
    catch (const GuestThreadExit& exit)
    {
        lucent::debug("thread", "guest thread {} terminated with code {:#x}",
            start->threadId, exit.exitCode);
    }

    lucent::info("thread", "guest thread {} exited", start->threadId);
    start->exited->Set();
}

} // namespace

// VOID ExTerminateThread(DWORD ExitCode) -- ends the calling thread. Never
// returns to guest code: the unwind lands in GuestThreadMain, which signals
// the thread's exit object so joiners wake.
void __imp__ExTerminateThread(PPCContext& __restrict ctx, uint8_t*)
{
    if (t_currentThread == nullptr)
    {
        // The primary thread was not made by ExCreateThread; it ending is the
        // process ending.
        lucent::warn("thread", "ExTerminateThread({:#x}) on the primary thread", ctx.r3.u32);
        std::exit(int(ctx.r3.u32));
    }
    throw GuestThreadExit{ctx.r3.u32};
}

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

    // The console takes the processor from the top byte of the creation flags,
    // as a one-hot mask; an empty mask means "wherever my creator runs". The
    // number is guest-visible -- title code indexes per-CPU tables with it -- so
    // it is recorded faithfully even though host threads are not pinned by it.
    const uint8_t requested = gears::ProcessorNumberFromMask(uint8_t(creationFlags >> 24));
    start->processor = requested == 0xFF ? gears::CurrentGuestProcessor() : requested;
    gears::SetGuestThreadProcessor(gears::Memory(), start->block.pcrAddress,
                                   start->block.threadAddress, start->processor);
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
    {
        std::lock_guard<std::mutex> guard(g_threadsMutex);
        g_threadsByHandle[handle] = start;
    }

    if (handlePtr != 0)
        *reinterpret_cast<uint32_t*>(base + handlePtr) = ByteSwap(handle);
    if (threadIdPtr != 0)
        *reinterpret_cast<uint32_t*>(base + threadIdPtr) = ByteSwap(start->threadId);

    lucent::info("thread", "ExCreateThread -> handle {:#x} id {} entry {:#x} stack {:#x}"
        " cpu {}{}", handle, start->threadId, startAddress, stackSize, start->processor,
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

// The title pins work to specific Xenon hardware threads.
//
// Where the HOST thread runs is still left to the host scheduler: pinning six
// guest threads onto six logical processors buys nothing on a machine with a
// different topology. What is honoured is the processor NUMBER, because that
// number is guest-visible state -- the audio worker indexes a per-CPU
// rendezvous array with it, and reading 0 there when the title assigned 4 left
// the barrier waiting on a slot nobody would ever fill (catalog #40).
void __imp__KeSetAffinityThread(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t affinity = ctx.r4.u32;
    const uint32_t previousPtr = ctx.r5.u32;

    const uint8_t cpu = gears::ProcessorNumberFromMask(uint8_t(affinity));
    const uint32_t handle = gears::HandleForGuestAddress(ctx.r3.u32);
    std::shared_ptr<GuestThreadStart> target;
    if (handle != 0)
    {
        std::lock_guard<std::mutex> guard(g_threadsMutex);
        if (auto it = g_threadsByHandle.find(handle); it != g_threadsByHandle.end())
            target = it->second;
    }

    if (target && cpu != 0xFF)
    {
        target->processor = cpu;
        gears::SetGuestThreadProcessor(gears::Memory(), target->block.pcrAddress,
                                       target->block.threadAddress, cpu);
        lucent::debug("thread", "KeSetAffinityThread(object={:#x}, mask={:#x})"
            " -> guest thread {} on cpu {}", ctx.r3.u32, affinity, target->threadId, cpu);
    }
    else
    {
        // Reported rather than ignored: a thread whose processor the runtime
        // could not place keeps whatever number it was created with, and any
        // per-CPU table the title builds for it will disagree.
        lucent::warn("thread", "KeSetAffinityThread(object={:#x}, mask={:#x}): no thread"
            " behind that object, processor left at its creation value",
            ctx.r3.u32, affinity);
    }

    if (previousPtr != 0)
        *reinterpret_cast<uint32_t*>(base + previousPtr) = ByteSwap(affinity);

    ctx.r3.u64 = affinity;
}

void __imp__KeSetBasePriorityThread(PPCContext& __restrict ctx, uint8_t*)
{
    // Host thread priorities need privileges we do not have and do not map
    // cleanly onto the console's scheme; the previous priority is reported so
    // the guest's save/restore pairs stay balanced.
    lucent::debug("thread", "KeSetBasePriorityThread({}) -- not honoured", int32_t(ctx.r4.u32));
    ctx.r3.u64 = 0;
}

void __imp__KeDelayExecutionThread(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t timeoutPtr = ctx.r5.u32;
    if (timeoutPtr == 0)
    {
        std::this_thread::yield();
        ctx.r3.u64 = gears::kStatusSuccess;
        return;
    }

    const int64_t raw = int64_t(ByteSwap(*reinterpret_cast<uint64_t*>(base + timeoutPtr)));
    if (raw > 0)
    {
        lucent::warn("thread", "absolute delay {} not supported, yielding instead", raw);
        std::this_thread::yield();
    }
    else
    {
        gears::PumpWaitScope probe("KeDelay");
        std::this_thread::sleep_for(std::chrono::nanoseconds(-raw * 100));
    }

    ctx.r3.u64 = gears::kStatusSuccess;
}
