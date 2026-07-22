// System notifications and the Xam message dispatch.
//
// The console posts notifications to titles for things this runtime has no
// source of: system UI opening and closing, sign-in changes, controller
// arrival, storage device changes. A listener is therefore real -- titles hold
// its handle and poll it -- but it is permanently empty, which is the same
// state a console produces when nothing has happened yet.
//
// That distinction matters: "no notification pending" is an ordinary answer a
// title must already cope with on every poll, whereas failing the listener
// creation outright is not a state the console produces.
#include "import_stub.h"

#include <mutex>
#include <unordered_map>

#include <lucent/log.h>

#include <byteswap.h>

#include "kernel_objects.h"
#include "xam_overlapped.h"

namespace
{
std::mutex g_listenerMutex;
std::unordered_map<uint32_t, uint64_t> g_listeners; // handle -> area mask
uint32_t g_nextListener = 0xFB000000;
} // namespace

// HANDLE XamNotifyCreateListener(ULONGLONG AreaMask)
void __imp__XamNotifyCreateListener(PPCContext& __restrict ctx, uint8_t*)
{
    const uint64_t mask = (uint64_t(ctx.r3.u32) << 32) | ctx.r4.u32;

    std::lock_guard<std::mutex> guard(g_listenerMutex);
    const uint32_t handle = g_nextListener;
    g_nextListener += 4;
    g_listeners[handle] = mask;

    lucent::debug("xam", "XamNotifyCreateListener(mask {:#x}) -> {:#x}", mask, handle);
    ctx.r3.u64 = handle;
}

// BOOL XNotifyGetNext(HANDLE Listener, DWORD MatchId, PDWORD Id, PULONG_PTR Param)
//
// FALSE means "nothing queued", which is what a title sees on most polls even
// on hardware, so returning it is an answer rather than an omission.
void __imp__XNotifyGetNext(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t idPtr = ctx.r5.u32;
    const uint32_t paramPtr = ctx.r6.u32;

    if (idPtr != 0)
        *reinterpret_cast<uint32_t*>(base + idPtr) = 0;
    if (paramPtr != 0)
        *reinterpret_cast<uint32_t*>(base + paramPtr) = 0;

    ctx.r3.u64 = 0; // FALSE
}

// VOID XNotifyPositionUI(DWORD Position)
//
// Where the system would draw its overlays. There are no overlays to place.
void __imp__XNotifyPositionUI(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("xam", "XNotifyPositionUI({:#x}) -- nothing to place", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// DWORD XMsgInProcessCall(DWORD App, DWORD Message, PVOID Arg1, PVOID Arg2)
//
// Dispatch into a Xam application. None are provided, so every message is
// genuinely unhandled and says so, rather than returning a success the caller
// would read as the service having run.
void __imp__XMsgInProcessCall(PPCContext& __restrict ctx, uint8_t*)
{
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
void __imp__XMsgStartIORequest(PPCContext& __restrict ctx, uint8_t* base)
{
    lucent::warn("xam", "XMsgStartIORequest(app {:#x}, message {:#x}) -- no such service",
        ctx.r3.u32, ctx.r4.u32);
    gears::CompleteOverlapped(base, ctx.r5.u32, gears::kErrorNotFound);
    ctx.r3.u64 = gears::kErrorNotFound;
}

void __imp__XMsgStartIORequestEx(PPCContext& __restrict ctx, uint8_t* base)
{
    lucent::warn("xam", "XMsgStartIORequestEx(app {:#x}, message {:#x}) -- no such service",
        ctx.r3.u32, ctx.r4.u32);
    gears::CompleteOverlapped(base, ctx.r5.u32, gears::kErrorNotFound);
    ctx.r3.u64 = gears::kErrorNotFound;
}

// A request that was never started has nothing to cancel, so this succeeds.
void __imp__XMsgCancelIORequest(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorSuccess;
}
