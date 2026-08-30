#include "guest_indirect_call.h"

#include "fault_report.h"
#include "guest_memory.h"
#include "guest_probe_state.h"

#include <atomic>

#include <lucent/log.h>

namespace gears::titles::gears1
{
namespace
{

inline constexpr std::uint32_t kLinkerCallStart = 0x824961D0;
inline constexpr std::uint32_t kLinkerCallEnd = 0x82496AE0;
inline constexpr std::uint32_t kStreamingGatherStart = 0x823ED7E0;
inline constexpr std::uint32_t kStreamingGatherEnd = 0x823EE600;
inline constexpr std::uint32_t kIndexArrayDataOffset = 768;
inline constexpr std::uint32_t kIndexArrayCountOffset = 772;
inline constexpr std::uint32_t kObjectTableOffset = 208;
inline constexpr std::uint32_t kObjectTableEntryBytes = 16;

[[nodiscard]] std::uint32_t ReadGuestBe32(std::uint8_t *base, std::uint32_t address)
{
    if (std::uint64_t(address) + sizeof(std::uint32_t) >= PPC_MEMORY_SIZE)
        return 0xDEADDEADu;
    return __builtin_bswap32(*reinterpret_cast<const std::uint32_t *>(base + address));
}

void ReportStreamingLookup(PPCContext &ctx, std::uint8_t *base)
{
    const std::uint32_t byteArray = ReadGuestBe32(base, ctx.r27.u32 + kIndexArrayDataOffset);
    const std::uint32_t count = ReadGuestBe32(base, ctx.r27.u32 + kIndexArrayCountOffset);
    const std::uint32_t table = ReadGuestBe32(base, ctx.r19.u32 + kObjectTableOffset);
    const std::uint32_t slot = ctx.r23.u32;
    const std::uint32_t index =
        byteArray != 0 && std::uint64_t(byteArray) + slot < PPC_MEMORY_SIZE
            ? *reinterpret_cast<const std::uint8_t *>(base + byteArray + slot)
            : 0xFFFFFFFFu;

    lucent::error("call",
                  "  #50 table lookup: byteArray {:#x}[{}] = index {}, count {}, table {:#x},"
                  " so the entry is at {:#x} and r22 = {:#x}",
                  byteArray, slot, index, count, table, table + index * kObjectTableEntryBytes,
                  ctx.r22.u32);
    lucent::error(
        "call", "  => {}",
        index != 0xFFFFFFFFu && index >= count
            ? "THE INDEX IS PAST THE COUNT, so the lookup read beyond the table and r22 is"
              " whatever lay after it"
            : "the index is WITHIN the count, so the table itself or its entry is wrong rather"
              " than the index -- do not blame the bounds");
}

void ReportBadIndirectCallContext(std::uint32_t, PPCContext &ctx, std::uint8_t *base)
{
    // The victim pointer alone does not name the Gears 1 subsystem that owns
    // it. These exact call ranges and probe reports are revision policy.
    if (ctx.lr >= kLinkerCallStart && ctx.lr < kLinkerCallEnd)
        ReportLinkerState(ctx.r24.u32);
    ReportMapNameProbe();
    ReportFStringProbe();
    ReportLoaderThunks();
    ReportEarlyThunks();

    // Issue #50: this gather maps one byte array through a separate object
    // table, checks only the 255 sentinel, and performs no table-length check.
    // Report the still-live lookup state; never clamp or continue.
    if (ctx.lr >= kStreamingGatherStart && ctx.lr < kStreamingGatherEnd)
        ReportStreamingLookup(ctx, base);
}

} // namespace

void NoteStreamingObject(PPCContext &ctx, std::uint8_t *base)
{
    const std::uint32_t object = ctx.r22.u32;
    const std::uint32_t firstWord =
        std::uint64_t(object) + sizeof(std::uint32_t) < PPC_MEMORY_SIZE
            ? __builtin_bswap32(*reinterpret_cast<const std::uint32_t *>(base + object))
            : 0xDEADDEADu;
    const bool plausible =
        firstWord >= PPC_IMAGE_BASE && firstWord < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;

    const std::uint32_t indexArray =
        std::uint64_t(ctx.r27.u32) + kIndexArrayCountOffset < PPC_MEMORY_SIZE
            ? ReadGuestBe32(base, ctx.r27.u32 + kIndexArrayDataOffset)
            : 0;
    const std::uint32_t index =
        indexArray != 0 && std::uint64_t(indexArray) + ctx.r23.u32 < PPC_MEMORY_SIZE
            ? *(base + indexArray + ctx.r23.u32)
            : 0xFFu;
    const std::uint32_t table =
        std::uint64_t(ctx.r19.u32) + kObjectTableOffset + sizeof(std::uint32_t) < PPC_MEMORY_SIZE
            ? ReadGuestBe32(base, ctx.r19.u32 + kObjectTableOffset)
            : 0;

    SetFaultContext("#50 (object, its first word, table index, table base):", object, firstWord,
                    index, table);

    static std::atomic<std::uint64_t> seen{0};
    static std::atomic<std::uint64_t> implausible{0};
    const std::uint64_t ordinal = seen.fetch_add(1) + 1;
    const std::uint64_t bad = plausible ? implausible.load() : implausible.fetch_add(1) + 1;
    if (ordinal == 1 || !plausible)
    {
        lucent::info(
            "call",
            "#50 streaming object #{}: {:#x}, first word {:#x} ({}); table {:#x} index {} ->"
            " entry {:#x}. {} of {} seen were not vtables.",
            ordinal, object, firstWord,
            plausible ? "in the image, so a plausible vtable"
                      : "NOT in the image -- this is the corruption",
            table, index, table + index * kObjectTableEntryBytes, bad, ordinal);
    }
}

} // namespace gears::titles::gears1

namespace gears
{

[[noreturn]] void ReportBadIndirectCall(std::uint32_t target, PPCContext &ctx, std::uint8_t *base)
{
    ReportGuestIndirectCallFailure(target, ctx, base,
                                   &titles::gears1::ReportBadIndirectCallContext);
}

} // namespace gears
