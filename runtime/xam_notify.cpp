// System notifications and the Xam message dispatch.
//
// System notifications and the Xam message dispatch. Listeners hold real
// filtered queues: system UI open/close is title-visible state, and dropping
// the close event leaves Gears' own modal on screen after a selector succeeds.
#include "import_stub.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

#include <lucent/log.h>

#include "xam_apps.h"

#include "byte_order.h"

#include "kernel_objects.h"
#include "xam_overlapped.h"
#include "xam_notify.h"

namespace
{
struct Notification
{
    uint32_t id;
    uint32_t param;
};

struct Listener
{
    uint64_t mask;
    uint32_t maxVersion;
    std::deque<Notification> pending;
};

struct ScheduledNotification
{
    std::chrono::steady_clock::time_point due;
    Notification value;
};

std::mutex g_listenerMutex;
std::unordered_map<uint32_t, Listener> g_listeners;
std::deque<ScheduledNotification> g_scheduled;
uint32_t g_nextListener = 0xFB000000;

uint32_t NotificationMaskIndex(uint32_t id)
{
    return (id >> 25) & 0x3F;
}

uint32_t NotificationVersion(uint32_t id)
{
    return (id >> 16) & 0x1FF;
}

bool Accepts(const Listener &listener, uint32_t id)
{
    const uint32_t index = NotificationMaskIndex(id);
    return index < 64 && (listener.mask & (uint64_t(1) << index)) != 0 &&
           NotificationVersion(id) <= listener.maxVersion;
}

void BroadcastLocked(uint32_t id, uint32_t param)
{
    size_t accepted = 0;
    for (auto &[handle, listener] : g_listeners)
    {
        (void)handle;
        if (!Accepts(listener, id))
            continue;
        listener.pending.push_back({id, param});
        ++accepted;
    }
    lucent::debug("xam", "notification {:#x} param {:#x}: {} listener(s) accepted", id, param,
                  accepted);
}

void MaterialiseScheduledLocked()
{
    const auto now = std::chrono::steady_clock::now();
    while (!g_scheduled.empty() && g_scheduled.front().due <= now)
    {
        const Notification value = g_scheduled.front().value;
        g_scheduled.pop_front();
        BroadcastLocked(value.id, value.param);
    }
}
} // namespace

namespace gears
{

void BroadcastNotification(uint32_t id, uint32_t param)
{
    std::lock_guard<std::mutex> guard(g_listenerMutex);
    MaterialiseScheduledLocked();
    BroadcastLocked(id, param);
}

void ScheduleNotification(uint32_t id, uint32_t param, std::chrono::milliseconds delay)
{
    std::lock_guard<std::mutex> guard(g_listenerMutex);
    MaterialiseScheduledLocked();
    const ScheduledNotification scheduled{std::chrono::steady_clock::now() + delay, {id, param}};
    const auto position =
        std::upper_bound(g_scheduled.begin(), g_scheduled.end(), scheduled.due,
                         [](const auto &due, const auto &item) { return due < item.due; });
    g_scheduled.insert(position, scheduled);
}

void ResetNotificationsForTest()
{
    std::lock_guard<std::mutex> guard(g_listenerMutex);
    g_listeners.clear();
    g_scheduled.clear();
    g_nextListener = 0xFB000000;
}

} // namespace gears

// HANDLE XamNotifyCreateListener(ULONGLONG AreaMask)
void __imp__XamNotifyCreateListener(PPCContext &__restrict ctx, uint8_t *)
{
    const uint64_t mask = (uint64_t(ctx.r3.u32) << 32) | ctx.r4.u32;
    const uint32_t maxVersion = std::min<uint32_t>(ctx.r5.u32, 10);

    std::lock_guard<std::mutex> guard(g_listenerMutex);
    const uint32_t handle = g_nextListener;
    g_nextListener += 4;
    g_listeners.emplace(handle, Listener{mask, maxVersion, {}});

    lucent::debug("xam", "XamNotifyCreateListener(mask {:#x}, max version {}) -> {:#x}", mask,
                  maxVersion, handle);
    ctx.r3.u64 = handle;
}

// BOOL XNotifyGetNext(HANDLE Listener, DWORD MatchId, PDWORD Id, PULONG_PTR Param)
//
// FALSE means "nothing queued", which is what a title sees on most polls even
// on hardware, so returning it is an answer rather than an omission.
void __imp__XNotifyGetNext(PPCContext &__restrict ctx, uint8_t *base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t matchId = ctx.r4.u32;
    const uint32_t idPtr = ctx.r5.u32;
    const uint32_t paramPtr = ctx.r6.u32;

    if (idPtr != 0)
        *reinterpret_cast<uint32_t *>(base + idPtr) = 0;
    if (paramPtr != 0)
        *reinterpret_cast<uint32_t *>(base + paramPtr) = 0;

    if (idPtr == 0)
    {
        ctx.r3.u64 = 0;
        return;
    }

    std::lock_guard<std::mutex> guard(g_listenerMutex);
    MaterialiseScheduledLocked();
    const auto found = g_listeners.find(handle);
    if (found == g_listeners.end())
    {
        ctx.r3.u64 = 0;
        return;
    }

    auto &pending = found->second.pending;
    auto item = pending.end();
    if (matchId == 0)
        item = pending.begin();
    else
        item = std::find_if(pending.begin(), pending.end(),
                            [matchId](const Notification &value) { return value.id == matchId; });

    if (item == pending.end())
    {
        ctx.r3.u64 = 0;
        return;
    }

    const Notification value = *item;
    pending.erase(item);
    *reinterpret_cast<uint32_t *>(base + idPtr) = ByteSwap(value.id);
    if (paramPtr != 0)
        *reinterpret_cast<uint32_t *>(base + paramPtr) = ByteSwap(value.param);
    ctx.r3.u64 = 1; // TRUE
}

// VOID XNotifyPositionUI(DWORD Position)
//
// Where the system would draw its overlays. There are no overlays to place.
void __imp__XNotifyPositionUI(PPCContext &__restrict ctx, uint8_t *)
{
    lucent::debug("xam", "XNotifyPositionUI({:#x}) -- nothing to place", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// DWORD XMsgInProcessCall(DWORD App, DWORD Message, PVOID Arg1, PVOID Arg2)
//
// Dispatch into a Xam application. Messages the runtime implements are handled
// here; anything else stays loud, so an unimplemented service is still visible
// rather than buried under a blanket success.
void __imp__XMsgInProcessCall(PPCContext &__restrict ctx, uint8_t *base)
{
    uint32_t status = 0;
    if (gears::DispatchXamMessage(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, status))
    {
        ctx.r3.u64 = status;
        return;
    }

    lucent::warn("xam", "XMsgInProcessCall(app {:#x}, message {:#x}) -- no such service",
                 ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = gears::kErrorNotFound;
}

// DWORD XMsgStartIORequest(DWORD App, DWORD Message, PXOVERLAPPED Overlapped,
//                          PVOID Buffer, DWORD Size)
//
// Asynchronous, so the refusal has to be delivered through the overlapped as
// well as returned. Returning an error alone left callers waiting on a request
// that never completed.
// The asynchronous form. The work is synchronous here, so the request is
// completed immediately -- but it MUST be completed either way, because a
// caller waiting on the overlapped has no other way to learn the outcome.
void __imp__XMsgStartIORequest(PPCContext &__restrict ctx, uint8_t *base)
{
    uint32_t status = 0;
    if (gears::DispatchXamMessage(base, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, status))
    {
        gears::CompleteOverlapped(base, ctx.r5.u32, status);
        ctx.r3.u64 = status;
        return;
    }

    lucent::warn("xam", "XMsgStartIORequest(app {:#x}, message {:#x}) -- no such service",
                 ctx.r3.u32, ctx.r4.u32);
    gears::CompleteOverlapped(base, ctx.r5.u32, gears::kErrorNotFound);
    ctx.r3.u64 = gears::kErrorNotFound;
}

void __imp__XMsgStartIORequestEx(PPCContext &__restrict ctx, uint8_t *base)
{
    uint32_t status = 0;
    if (gears::DispatchXamMessage(base, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, status))
    {
        gears::CompleteOverlapped(base, ctx.r5.u32, status);
        ctx.r3.u64 = status;
        return;
    }

    lucent::warn("xam", "XMsgStartIORequestEx(app {:#x}, message {:#x}) -- no such service",
                 ctx.r3.u32, ctx.r4.u32);
    gears::CompleteOverlapped(base, ctx.r5.u32, gears::kErrorNotFound);
    ctx.r3.u64 = gears::kErrorNotFound;
}

// A request that was never started has nothing to cancel, so this succeeds.
void __imp__XMsgCancelIORequest(PPCContext &__restrict ctx, uint8_t *)
{
    ctx.r3.u64 = gears::kErrorSuccess;
}
