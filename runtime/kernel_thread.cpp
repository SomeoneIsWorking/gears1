// Guest thread creation.
//
// Retained Xbox thread-service semantics. Guest entry execution belongs to
// x360port/Xenia and currently refuses at that explicit missing boundary.
#include "import_stub.h"

#include "fatal_exit.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <thread>

#include "byte_order.h"
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_memory.h"
#include "fault_report.h"
#include "guest_thread.h"
#include "wait_probe.h"
#include "kernel_objects.h"
#include "missing_x360port_executor.h"

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

// Threads by the KERNEL OBJECT behind them, so an API that names a thread by
// the object pointer the title holds can get back to the block the runtime
// built for it.
//
// It used to be keyed by the handle ExCreateThread minted, and that is wrong
// because a handle is not a thread's identity: the title duplicates it. The
// wrapper at 0x82942164 creates the thread whose entry point is 0x82941df0,
// then at 0x8294218C calls DuplicateHandle (guest sub_8294F198 -> our
// NtDuplicateObject) on the handle it got back -- and it is the DUPLICATE that
// XSetThreadProcessor (guest sub_82613900) resolves with
// ObReferenceObjectByHandle and hands to KeSetAffinityThread. Keyed by handle
// the duplicate matched nothing, so that thread's pin was silently dropped in
// every run ("no thread behind that object" in scratch/logs/phys2.log:179-181,
// which shows the NtDuplicateObject, the ObReferenceObjectByHandle of the
// duplicate, and the failed pin on consecutive lines).
//
// A duplicated handle names the SAME host object, so the object is the identity
// that survives duplication. The shared_ptr in the record keeps it alive, so a
// key is never reused by a later object at the same address.
std::mutex g_threadsMutex;
std::unordered_map<const gears::KernelObject *, std::shared_ptr<GuestThreadStart>>
    g_threadsByObject;

std::shared_ptr<GuestThreadStart> ThreadForObject(const gears::KernelObject *object)
{
    if (object == nullptr)
        return nullptr;
    std::lock_guard<std::mutex> guard(g_threadsMutex);
    auto it = g_threadsByObject.find(object);
    return it != g_threadsByObject.end() ? it->second : nullptr;
}

// Thrown by ExTerminateThread to unwind the calling guest thread back to
// GuestThreadMain. Guest execution can leave host-side objects with no cleanup of
// their own, so the unwind is safe; only the runtime's own frames run
// destructors on the way out.
struct GuestThreadExit
{
    uint32_t exitCode;
};

thread_local GuestThreadStart *t_currentThread = nullptr;

void GuestThreadMain(std::shared_ptr<GuestThreadStart> start)
{
    // A suspended thread must not run guest code until it is resumed.
    if (start->resumed)
        start->resumed->Wait(-1);

    t_currentThread = start.get();
    gears::SetGuestThreadName("guest-" + std::to_string(start->threadId));
    // Every guest thread gets its own signal stack, because the faults worth
    // reporting happen on these threads and a handler with nowhere to run
    // reports nothing at all.
    gears::InstallSignalStackForThisThread();
    // So threads this one creates without a processor of their own inherit it,
    // which is what the console does.
    gears::SetCurrentGuestProcessor(start->processor);

    PPCContext ctx{};
    ctx.r13.u32 = start->block.pcrAddress;
    ctx.r1.u32 = start->block.stackBase - 0x100;
    ctx.fpscr.loadFromHost();

    lucent::info("thread", "guest thread {} entering {:#x} (context {:#x})", start->threadId,
                 start->startAddress, start->startContext);

    try
    {
        // The XAPI startup shim, when present, is what the console calls; it
        // takes the real entry point and its argument and handles teardown.
        if (start->startupRoutine != 0)
        {
            ctx.r3.u32 = start->startAddress;
            ctx.r4.u32 = start->startContext;
            gears::RefuseMissingX360PortExecutor(start->startupRoutine);
        }
        else
        {
            ctx.r3.u32 = start->startContext;
            gears::RefuseMissingX360PortExecutor(start->startAddress);
        }
    }
    catch (const GuestThreadExit &exit)
    {
        lucent::debug("thread", "guest thread {} terminated with code {:#x}", start->threadId,
                      exit.exitCode);
    }

    lucent::info("thread", "guest thread {} exited", start->threadId);
    start->exited->Set();
}

} // namespace

// VOID ExTerminateThread(DWORD ExitCode) -- ends the calling thread. Never
// returns to guest code: the unwind lands in GuestThreadMain, which signals
// the thread's exit object so joiners wake.
void __imp__ExTerminateThread(PPCContext &__restrict ctx, uint8_t *)
{
    if (t_currentThread == nullptr)
    {
        // The primary thread was not made by ExCreateThread; it ending is the
        // process ending.
        lucent::warn("thread", "ExTerminateThread({:#x}) on the primary thread", ctx.r3.u32);
        gears::FatalExit(int(ctx.r3.u32));
    }
    throw GuestThreadExit{ctx.r3.u32};
}

// NTSTATUS ExCreateThread(PHANDLE Handle, ULONG StackSize, PULONG ThreadId,
//                         PVOID XapiThreadStartup, PVOID StartAddress,
//                         PVOID StartContext, ULONG CreationFlags)
void __imp__ExCreateThread(PPCContext &__restrict ctx, uint8_t *base)
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
    const uint8_t requested = gears::ProcessorNumberFromMask(creationFlags >> 24);
    // A mask that names nothing has told us as little as an empty one, and the
    // conversion has already said so, so both fall back to the creator's
    // processor rather than to an invented number.
    start->processor =
        requested >= gears::kHardwareThreadCount ? gears::CurrentGuestProcessor() : requested;
    gears::SetGuestThreadProcessor(gears::Memory(), start->block.pcrAddress,
                                   start->block.threadAddress, start->processor);
    start->exited =
        std::make_shared<gears::KernelObject>(gears::KernelObject::Kind::NotificationEvent, false);

    if ((creationFlags & kCreateSuspended) != 0)
    {
        start->resumed = std::make_shared<gears::KernelObject>(
            gears::KernelObject::Kind::NotificationEvent, false);
    }

    // Registered BEFORE the handle exists and before the thread is started, so
    // there is no window in which the title holds a name for a thread the
    // runtime cannot resolve.
    {
        std::lock_guard<std::mutex> guard(g_threadsMutex);
        g_threadsByObject[start->exited.get()] = start;
    }

    // The handle waits on thread exit, which is what the guest joins against.
    const uint32_t handle = gears::Handles().Insert(start->exited);
    gears::RegisterThreadResume(start->exited, start->resumed);

    if (handlePtr != 0)
        *reinterpret_cast<uint32_t *>(base + handlePtr) = ByteSwap(handle);
    if (threadIdPtr != 0)
        *reinterpret_cast<uint32_t *>(base + threadIdPtr) = ByteSwap(start->threadId);

    lucent::info("thread",
                 "ExCreateThread -> handle {:#x} id {} entry {:#x} stack {:#x}"
                 " cpu {}{}",
                 handle, start->threadId, startAddress, stackSize, start->processor,
                 start->resumed ? " (suspended)" : "");

    std::thread(GuestThreadMain, start).detach();
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__NtResumeThread(PPCContext &__restrict ctx, uint8_t *)
{
    gears::ResumeThread(ctx.r3.u32);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__KeResumeThread(PPCContext &__restrict ctx, uint8_t *)
{
    gears::ResumeThread(ctx.r3.u32);
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS KeSetAffinityThread(PKTHREAD Thread, KAFFINITY Affinity,
//                              PKAFFINITY PreviousAffinity)
//
// The title pins work to specific Xenon hardware threads.
//
// Where the HOST thread runs is still left to the host scheduler: pinning six
// guest threads onto six logical processors buys nothing on a machine with a
// different topology. What is honoured is the processor NUMBER, because that
// number is guest-visible state -- the audio worker indexes a per-CPU
// rendezvous array with it, and reading 0 there when the title assigned 4 left
// the barrier waiting on a slot nobody would ever fill (catalog #40).
//
// Unlike Windows NT, the console's KeSetAffinityThread returns an NTSTATUS and
// reports the PREVIOUS affinity through the pointer argument -- Xenia says so
// from disassembling the real export (xboxkrnl_threading.cc:359). This used to
// return the requested mask in r3 and echo it back through the pointer, which
// told the caller nothing and hid the failures below.
void __imp__KeSetAffinityThread(PPCContext &__restrict ctx, uint8_t *base)
{
    const uint32_t threadObject = ctx.r3.u32;
    const uint32_t affinity = ctx.r4.u32;
    const uint32_t previousPtr = ctx.r5.u32;

    if (affinity == 0)
    {
        // The console rejects an empty affinity outright; unlike a thread's
        // CREATION flags, where an empty mask means "inherit", there is no
        // creator here to inherit from.
        lucent::warn("thread",
                     "KeSetAffinityThread(object={:#x}): an empty affinity"
                     " names no processor",
                     threadObject);
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    // The thread is named by a POINTER to its dispatcher object, which the guest
    // obtained from ObReferenceObjectByHandle -- so the address resolves to the
    // same host object no matter which of its handles it came from, including a
    // duplicate. Looking the handle up instead is what lost the pins.
    auto target = ThreadForObject(gears::LookupByGuestAddress(threadObject).get());
    if (!target)
    {
        // Reported rather than ignored: a thread whose processor the runtime
        // could not place keeps whatever number it was created with, and any
        // per-CPU table the title builds for it will disagree.
        lucent::warn("thread",
                     "KeSetAffinityThread(object={:#x}, mask={:#x}): that"
                     " object is not a thread the runtime created, processor left alone",
                     threadObject, affinity);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    // One conversion, shared with ExCreateThread's creation flags, so the two
    // ways the title names a processor cannot come to disagree.
    const uint8_t cpu = gears::ProcessorNumberFromMask(affinity);
    if (cpu >= gears::kHardwareThreadCount)
    {
        // ProcessorNumberFromMask has already said what is wrong with the mask.
        // Nothing can be inferred from it, so the thread stays where it is.
        //
        // NOT writing PreviousAffinity here, deliberately. Nothing establishes
        // that the console writes an output on a failing status, and the one
        // failure path that IS established -- an empty affinity, above --
        // returns without writing. Two failure paths that behave differently
        // would be a guess dressed as a contract; if a title is later seen to
        // depend on the write, that observation is what should change this.
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    if (previousPtr != 0)
    {
        *reinterpret_cast<uint32_t *>(base + previousPtr) =
            ByteSwap(uint32_t(1) << target->processor);
    }

    target->processor = cpu;
    gears::SetGuestThreadProcessor(gears::Memory(), target->block.pcrAddress,
                                   target->block.threadAddress, cpu);
    // A thread that pins ITSELF -- which is what the title's worker at
    // 0x82941df0 does -- must also move the value its own children inherit. That
    // lives in a thread_local, so only this thread can write it, and only when
    // it is its own target.
    if (t_currentThread == target.get())
        gears::SetCurrentGuestProcessor(cpu);
    lucent::debug("thread",
                  "KeSetAffinityThread(object={:#x}, mask={:#x})"
                  " -> guest thread {} on cpu {}",
                  threadObject, affinity, target->threadId, cpu);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__KeSetBasePriorityThread(PPCContext &__restrict ctx, uint8_t *)
{
    // Host thread priorities need privileges we do not have and do not map
    // cleanly onto the console's scheme; the previous priority is reported so
    // the guest's save/restore pairs stay balanced.
    lucent::debug("thread", "KeSetBasePriorityThread({}) -- not honoured", int32_t(ctx.r4.u32));
    ctx.r3.u64 = 0;
}

void __imp__KeDelayExecutionThread(PPCContext &__restrict ctx, uint8_t *base)
{
    const uint32_t timeoutPtr = ctx.r5.u32;
    if (timeoutPtr == 0)
    {
        std::this_thread::yield();
        ctx.r3.u64 = gears::kStatusSuccess;
        return;
    }

    const int64_t raw = int64_t(ByteSwap(*reinterpret_cast<uint64_t *>(base + timeoutPtr)));
    if (raw > 0)
    {
        lucent::warn("thread", "absolute delay {} not supported, yielding instead", raw);
        std::this_thread::yield();
    }
    else
    {
        gears::WaitProbe probe("KeDelay");
        std::this_thread::sleep_for(std::chrono::nanoseconds(-raw * 100));
    }

    ctx.r3.u64 = gears::kStatusSuccess;
}
