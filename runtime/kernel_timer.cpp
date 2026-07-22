// Kernel timers.
//
// A timer is an event that signals itself when its due time arrives, so it
// reuses KernelObject rather than becoming a fourth kind: a notification timer
// stays signalled until reset, a synchronisation timer releases one waiter,
// which is exactly the existing event distinction.
//
// One scheduler thread owns every armed timer. A thread per timer would be
// simpler to write and worse to run, and it would make the ordering of two
// timers due at the same moment depend on the host scheduler.
#include "import_stub.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <lucent/log.h>

#include <byteswap.h>

#include "kernel_objects.h"

namespace
{
using Clock = std::chrono::steady_clock;

struct ArmedTimer
{
    uint32_t handle;
    std::shared_ptr<gears::KernelObject> object;
    Clock::time_point due;
    uint32_t periodMilliseconds; // 0 for a one-shot
};

std::mutex g_mutex;
std::condition_variable g_wakeup;
std::vector<ArmedTimer> g_armed;
bool g_schedulerStarted = false;

void Scheduler()
{
    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;)
    {
        if (g_armed.empty())
        {
            g_wakeup.wait(lock);
            continue;
        }

        const auto next = std::min_element(g_armed.begin(), g_armed.end(),
            [](const ArmedTimer& a, const ArmedTimer& b) { return a.due < b.due; });

        const Clock::time_point due = next->due;
        if (Clock::now() < due)
        {
            // Re-checked after waking rather than assumed: arming another timer
            // can bring an earlier due time in while this one waits.
            g_wakeup.wait_until(lock, due);
            continue;
        }

        auto object = next->object;
        if (next->periodMilliseconds != 0)
            next->due = due + std::chrono::milliseconds(next->periodMilliseconds);
        else
            g_armed.erase(next);

        // Signalled outside the lock: a released waiter may arm a timer of its
        // own, and holding the lock across that would deadlock.
        lock.unlock();
        object->Set();
        lock.lock();
    }
}

void EnsureScheduler()
{
    if (g_schedulerStarted)
        return;
    g_schedulerStarted = true;
    std::thread(Scheduler).detach();
}

void Disarm(uint32_t handle)
{
    g_armed.erase(std::remove_if(g_armed.begin(), g_armed.end(),
        [handle](const ArmedTimer& t) { return t.handle == handle; }), g_armed.end());
}
} // namespace

// NTSTATUS NtCreateTimer(PHANDLE Handle, POBJECT_ATTRIBUTES Attributes, DWORD Type)
//
// Type 0 is a notification timer, 1 a synchronisation timer -- the same
// distinction events draw, so the same two kinds serve.
void __imp__NtCreateTimer(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handlePtr = ctx.r3.u32;
    const uint32_t type = ctx.r5.u32;

    const auto kind = type == 1
        ? gears::KernelObject::Kind::SynchronizationEvent
        : gears::KernelObject::Kind::NotificationEvent;

    auto object = std::make_shared<gears::KernelObject>(kind, false);
    const uint32_t handle = gears::Handles().Insert(object);

    if (handlePtr != 0)
        *reinterpret_cast<uint32_t*>(base + handlePtr) = ByteSwap(handle);

    lucent::debug("timer", "NtCreateTimer(type {}) -> {:#x}", type, handle);
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS NtSetTimerEx(HANDLE Timer, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE Apc,
//                       DWORD ApcMode, PVOID ApcContext, BOOLEAN Resume, LONG Period,
//                       PBOOLEAN PreviousState)
void __imp__NtSetTimerEx(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t dueTimePtr = ctx.r4.u32;
    const uint32_t apcRoutine = ctx.r5.u32;
    const uint32_t period = ctx.r9.u32;
    const uint32_t previousStatePtr = ctx.r10.u32;

    auto object = gears::Handles().Lookup(handle);
    if (!object)
    {
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    if (apcRoutine != 0)
    {
        // APCs are not delivered. Said out loud rather than ignored: a title
        // relying on the callback would otherwise simply never be called and
        // the reason would not appear anywhere.
        lucent::warn("timer", "NtSetTimerEx({:#x}) asked for APC {:#x}, which is not delivered",
            handle, apcRoutine);
    }

    // A negative due time is relative in 100 ns units; a positive one is an
    // absolute file time. Only the relative form has appeared, and guessing an
    // epoch for the absolute form would be worse than refusing it.
    int64_t dueTime = 0;
    if (dueTimePtr != 0)
        dueTime = int64_t(ByteSwap(*reinterpret_cast<uint64_t*>(base + dueTimePtr)));

    Clock::duration delay{};
    if (dueTime < 0)
    {
        delay = std::chrono::nanoseconds(-dueTime * 100);
    }
    else if (dueTime > 0)
    {
        lucent::warn("timer", "NtSetTimerEx({:#x}) used an absolute due time, treated as immediate",
            handle);
    }

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        EnsureScheduler();
        Disarm(handle); // re-arming replaces the previous schedule
        g_armed.push_back({ handle, object, Clock::now() + delay, period });
    }
    g_wakeup.notify_all();

    object->Clear();
    if (previousStatePtr != 0)
        *reinterpret_cast<uint32_t*>(base + previousStatePtr) = 0;

    lucent::debug("timer", "NtSetTimerEx({:#x}) in {} ms, period {} ms", handle,
        std::chrono::duration_cast<std::chrono::milliseconds>(delay).count(), period);
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS NtCancelTimer(HANDLE Timer, PBOOLEAN CurrentState)
void __imp__NtCancelTimer(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t currentStatePtr = ctx.r4.u32;

    bool wasArmed;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        const size_t before = g_armed.size();
        Disarm(handle);
        wasArmed = g_armed.size() != before;
    }
    g_wakeup.notify_all();

    if (currentStatePtr != 0)
        *reinterpret_cast<uint32_t*>(base + currentStatePtr) = wasArmed ? 1 : 0;

    ctx.r3.u64 = gears::kStatusSuccess;
}
