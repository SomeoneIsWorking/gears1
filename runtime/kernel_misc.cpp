// Privileges, pool allocation and thread-local storage.
#include "import_stub.h"

#include <array>
#include <atomic>
#include <cstring>
#include <string_view>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "import_variables.h"

namespace
{
constexpr size_t kTlsSlotCount = 64;

// Per guest thread once guest threads exist; per host thread today, which is
// the same thing while the runtime is still single-threaded.
thread_local std::array<uint32_t, kTlsSlotCount> t_tlsSlots{};
std::atomic<size_t> g_nextTlsSlot{0};
} // namespace

// Titles query privileges (online play, media playback, ...) to enable optional
// features. Reporting none is honest for a runtime with no Live backing, and
// the game is expected to cope.
void __imp__XexCheckExecutablePrivilege(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("kernel", "XexCheckExecutablePrivilege({}) -> 0", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

void __imp__ExAllocatePool(PPCContext& __restrict ctx, uint8_t*)
{
    uint32_t size = ctx.r3.u32;
    ctx.r3.u64 = gears::TitleHeap().Allocate(0, size, gears::kMemCommit);
}

void __imp__ExAllocatePoolWithTag(PPCContext& __restrict ctx, uint8_t*)
{
    uint32_t size = ctx.r3.u32;
    ctx.r3.u64 = gears::TitleHeap().Allocate(0, size, gears::kMemCommit);
}

void __imp__ExFreePool(PPCContext& __restrict ctx, uint8_t*)
{
    gears::TitleHeap().Free(ctx.r3.u32);
}

void __imp__KeTlsAlloc(PPCContext& __restrict ctx, uint8_t*)
{
    const size_t slot = g_nextTlsSlot.fetch_add(1);
    if (slot >= kTlsSlotCount)
    {
        lucent::error("kernel", "KeTlsAlloc exhausted {} slots", kTlsSlotCount);
        ctx.r3.u64 = uint32_t(-1); // TLS_OUT_OF_INDEXES
        return;
    }

    lucent::debug("kernel", "KeTlsAlloc -> slot {}", slot);
    ctx.r3.u64 = uint32_t(slot);
}

void __imp__KeTlsFree(PPCContext& __restrict ctx, uint8_t*)
{
    // Slots are never recycled: a title allocates a handful at startup and
    // holds them for its lifetime, so a free list would be machinery with no
    // caller. Freeing one leaves it readable-but-unused.
    ctx.r3.u64 = 1;
}

void __imp__KeTlsGetValue(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t slot = ctx.r3.u32;
    ctx.r3.u64 = slot < kTlsSlotCount ? t_tlsSlots[slot] : 0;
}

void __imp__KeTlsSetValue(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t slot = ctx.r3.u32;
    if (slot < kTlsSlotCount)
        t_tlsSlots[slot] = ctx.r4.u32;
    ctx.r3.u64 = slot < kTlsSlotCount ? 1 : 0;
}

void __imp__DbgPrint(PPCContext& __restrict ctx, uint8_t* base)
{
    // The format arguments follow the PPC varargs convention; rendering them
    // properly needs the guest stack walked, so for now the format string alone
    // is reported -- enough to see what the title is complaining about.
    const char* format = reinterpret_cast<const char*>(base + ctx.r3.u32);
    lucent::info("guest", "DbgPrint: {}", format);
}

// The console's file-system cache tuning knob. There is no equivalent here and
// nothing depends on the size, so the title's request is simply accepted.
void __imp__FscSetCacheElementCount(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("kernel", "FscSetCacheElementCount({}) -> success", ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// The title registers a routine to run when it is being torn down. Nothing
// tears a title down here yet, so the registration is recorded and the routine
// is never invoked.
void __imp__ExRegisterTitleTerminateNotification(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("kernel", "ExRegisterTitleTerminateNotification(routine={:#x}, create={})",
        ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// A deliberately empty APC routine the kernel exports so callers have
// something valid to point at. Doing nothing is the whole contract.
void __imp__KiApcNormalRoutineNop(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

// NTSTATUS XexGetModuleHandle(PCSTR moduleName, PHANDLE outHandle)
//
// A NULL name asks for the running executable. Only that case can be answered
// honestly here: no other modules are loaded, so any name lookup genuinely has
// nothing to find and reporting that is correct rather than a stand-in.
void __imp__XexGetModuleHandle(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t nameAddress = ctx.r3.u32;
    const uint32_t outAddress = ctx.r4.u32;

    if (nameAddress != 0)
    {
        lucent::warn("kernel", "XexGetModuleHandle({}) -> not found",
            reinterpret_cast<const char*>(base + nameAddress));
        ctx.r3.u64 = gears::kStatusNotFound;
        return;
    }

    if (outAddress == 0)
    {
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    const uint32_t handle = gears::ExecutableModuleHandle();
    *reinterpret_cast<uint32_t*>(base + outAddress) = ByteSwap(handle);

    lucent::debug("kernel", "XexGetModuleHandle(NULL) -> {:#x}", handle);
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS XexGetModuleSection(HANDLE module, PCSTR name,
//                              PVOID* outStart, PDWORD outLength)
//
// The XEX's PE section table is already parsed by the loader, so this is a
// real lookup rather than a stand-in: a section the image genuinely lacks
// reports not-found, which is the same answer hardware would give.
void __imp__XexGetModuleSection(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t nameAddress = ctx.r4.u32;
    const uint32_t outStart = ctx.r5.u32;
    const uint32_t outLength = ctx.r6.u32;

    if (handle != gears::ExecutableModuleHandle())
    {
        lucent::warn("kernel", "XexGetModuleSection: unknown module handle {:#x}", handle);
        ctx.r3.u64 = gears::kStatusInvalidHandle;
        return;
    }

    const Image* image = gears::LoadedImage();
    if (image == nullptr || nameAddress == 0 || outStart == 0 || outLength == 0)
    {
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    // PE section names are eight bytes and only NUL-terminated when shorter,
    // so the guest string is bounded rather than read to the first NUL.
    const char* requested = reinterpret_cast<const char*>(base + nameAddress);
    const std::string_view name(requested, strnlen(requested, 8));

    const Section* section = image->Find(name);
    if (section == nullptr)
    {
        lucent::warn("kernel", "XexGetModuleSection({}) -> not present in the image", name);
        ctx.r3.u64 = gears::kStatusNotFound;
        return;
    }

    *reinterpret_cast<uint32_t*>(base + outStart) = ByteSwap(uint32_t(section->base));
    *reinterpret_cast<uint32_t*>(base + outLength) = ByteSwap(section->size);

    lucent::debug("kernel", "XexGetModuleSection({}) -> {:#x}+{:#x}",
        name, uint32_t(section->base), section->size);
    ctx.r3.u64 = gears::kStatusSuccess;
}
