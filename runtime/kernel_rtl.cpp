// Counted-string helpers and status translation.
#include "import_stub.h"

#include <chrono>
#include <cstring>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"

namespace
{

// X_ANSI_STRING and X_UNICODE_STRING share this shape; only the element width
// of the buffer differs.
struct GuestCountedString
{
    uint16_t length;        // in bytes, excluding the terminator
    uint16_t maximumLength; // in bytes, including the terminator
    uint32_t buffer;
};

GuestCountedString* StringAt(uint8_t* base, uint32_t address)
{
    return reinterpret_cast<GuestCountedString*>(base + address);
}

size_t WideLength(const uint8_t* p)
{
    size_t n = 0;
    while (p[n * 2] != 0 || p[n * 2 + 1] != 0)
        ++n;
    return n;
}

} // namespace

void __imp__RtlInitAnsiString(PPCContext& __restrict ctx, uint8_t* base)
{
    GuestCountedString* dest = StringAt(base, ctx.r3.u32);
    const uint32_t source = ctx.r4.u32;

    if (source == 0)
    {
        dest->length = 0;
        dest->maximumLength = 0;
        dest->buffer = 0;
        return;
    }

    const size_t length = strlen(reinterpret_cast<const char*>(base + source));
    dest->length = ByteSwap(uint16_t(length));
    dest->maximumLength = ByteSwap(uint16_t(length + 1));
    dest->buffer = ByteSwap(source);
}

void __imp__RtlInitUnicodeString(PPCContext& __restrict ctx, uint8_t* base)
{
    GuestCountedString* dest = StringAt(base, ctx.r3.u32);
    const uint32_t source = ctx.r4.u32;

    if (source == 0)
    {
        dest->length = 0;
        dest->maximumLength = 0;
        dest->buffer = 0;
        return;
    }

    const size_t characters = WideLength(base + source);
    dest->length = ByteSwap(uint16_t(characters * 2));
    dest->maximumLength = ByteSwap(uint16_t((characters + 1) * 2));
    dest->buffer = ByteSwap(source);
}

void __imp__RtlFreeAnsiString(PPCContext& __restrict ctx, uint8_t* base)
{
    GuestCountedString* target = StringAt(base, ctx.r3.u32);
    if (target->buffer != 0)
        gears::TitleHeap().Free(ByteSwap(target->buffer));

    target->length = 0;
    target->maximumLength = 0;
    target->buffer = 0;
}

// NTSTATUS RtlUnicodeToMultiByteN(PCHAR Dest, ULONG DestSize, PULONG BytesWritten,
//                                 PCWCH Source, ULONG SourceSize)
// The console's conversion is code-page based; the titles reaching this path use
// it for ASCII file paths, so anything above 0x7F is replaced rather than
// silently truncated to a wrong character.
void __imp__RtlUnicodeToMultiByteN(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t dest = ctx.r3.u32;
    const uint32_t destSize = ctx.r4.u32;
    const uint32_t writtenPtr = ctx.r5.u32;
    const uint32_t source = ctx.r6.u32;
    const uint32_t sourceSizeBytes = ctx.r7.u32;

    const uint32_t characters = sourceSizeBytes / 2;
    const uint32_t count = characters < destSize ? characters : destSize;

    for (uint32_t i = 0; i < count; i++)
    {
        const uint16_t wide = ByteSwap(*reinterpret_cast<uint16_t*>(base + source + i * 2));
        base[dest + i] = wide < 0x80 ? uint8_t(wide) : '?';
    }

    if (writtenPtr != 0)
        *reinterpret_cast<uint32_t*>(base + writtenPtr) = ByteSwap(count);

    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__RtlMultiByteToUnicodeN(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t dest = ctx.r3.u32;
    const uint32_t destSizeBytes = ctx.r4.u32;
    const uint32_t writtenPtr = ctx.r5.u32;
    const uint32_t source = ctx.r6.u32;
    const uint32_t sourceSize = ctx.r7.u32;

    const uint32_t capacity = destSizeBytes / 2;
    const uint32_t count = sourceSize < capacity ? sourceSize : capacity;

    for (uint32_t i = 0; i < count; i++)
    {
        *reinterpret_cast<uint16_t*>(base + dest + i * 2) = ByteSwap(uint16_t(base[source + i]));
    }

    if (writtenPtr != 0)
        *reinterpret_cast<uint32_t*>(base + writtenPtr) = ByteSwap(count * 2);

    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__RtlNtStatusToDosError(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t status = ctx.r3.u32;
    if (status == 0)
    {
        ctx.r3.u64 = 0;
        return;
    }

    // A faithful table is large and none of it is reached yet; ERROR_GEN_FAILURE
    // is a truthful "something went wrong" rather than a specific wrong answer.
    lucent::debug("kernel", "RtlNtStatusToDosError({:#x}) -> ERROR_GEN_FAILURE", status);
    ctx.r3.u64 = 31;
}

// VOID RtlFillMemoryUlong(PVOID Destination, SIZE_T Length, ULONG Pattern)
// Length is in bytes but always a multiple of four; the pattern is written as a
// big-endian guest word, so the stored bytes match what the guest would write.
void __imp__RtlFillMemoryUlong(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t dest = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32;
    const uint32_t pattern = ByteSwap(ctx.r5.u32);

    auto* out = reinterpret_cast<uint32_t*>(base + dest);
    for (uint32_t i = 0; i < length / 4; i++)
        out[i] = pattern;
}

// SIZE_T RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern)
// Returns the number of leading bytes that match the pattern.
void __imp__RtlCompareMemoryUlong(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t source = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32;
    const uint32_t pattern = ByteSwap(ctx.r5.u32);

    const auto* in = reinterpret_cast<const uint32_t*>(base + source);
    uint32_t matched = 0;
    for (uint32_t i = 0; i < length / 4; i++)
    {
        if (in[i] != pattern)
            break;
        matched += 4;
    }

    ctx.r3.u64 = matched;
}

namespace
{
// X_TIME_FIELDS: eight big-endian 16-bit fields, in this order.
struct GuestTimeFields
{
    uint16_t year;
    uint16_t month;        // 1..12
    uint16_t day;          // 1..31
    uint16_t hour;         // 0..23
    uint16_t minute;       // 0..59
    uint16_t second;       // 0..59
    uint16_t milliseconds; // 0..999
    uint16_t weekday;      // 0..6, 0 = Sunday
};

// Both directions convert against a plain calendar with no timezone or clock
// scaling, exactly as the RTL routines do: the value is 100 ns ticks since the
// 1601-01-01 Windows epoch, and std::chrono::system_clock uses the 1970 Unix
// epoch, so the two differ by a fixed constant.
constexpr int64_t kEpochDelta100ns = 116444736000000000; // 1601-01-01 -> 1970-01-01

using days = std::chrono::duration<int64_t, std::ratio<86400>>;
} // namespace

// VOID RtlTimeToTimeFields(PLARGE_INTEGER Time, PTIME_FIELDS TimeFields)
void __imp__RtlTimeToTimeFields(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t timeAddress = ctx.r3.u32;
    const uint32_t fieldsAddress = ctx.r4.u32;
    if (timeAddress == 0 || fieldsAddress == 0)
        return;

    const int64_t ticks1601 = int64_t(ByteSwap(*reinterpret_cast<uint64_t*>(base + timeAddress)));
    const int64_t ticksUnix = ticks1601 - kEpochDelta100ns;

    // Split into whole days and the time within the day, flooring so that a
    // negative pre-1970 tick count still lands on the correct calendar day.
    const auto totalNs = std::chrono::duration<int64_t, std::ratio<1, 10000000>>(ticksUnix);
    const auto sinceEpoch = std::chrono::floor<std::chrono::milliseconds>(totalNs);
    const auto dayPart = std::chrono::floor<days>(sinceEpoch);
    const auto timeOfDay = sinceEpoch - dayPart;

    const std::chrono::year_month_day ymd{std::chrono::sys_days{dayPart}};
    const std::chrono::hh_mm_ss<std::chrono::milliseconds> hms{
        std::chrono::duration_cast<std::chrono::milliseconds>(timeOfDay)};
    const std::chrono::weekday weekday{std::chrono::sys_days{dayPart}};

    GuestTimeFields out;
    out.year = ByteSwap(uint16_t(int(ymd.year())));
    out.month = ByteSwap(uint16_t(unsigned(ymd.month())));
    out.day = ByteSwap(uint16_t(unsigned(ymd.day())));
    out.hour = ByteSwap(uint16_t(hms.hours().count()));
    out.minute = ByteSwap(uint16_t(hms.minutes().count()));
    out.second = ByteSwap(uint16_t(hms.seconds().count()));
    out.milliseconds = ByteSwap(uint16_t(hms.subseconds().count()));
    out.weekday = ByteSwap(uint16_t(weekday.c_encoding())); // 0 = Sunday
    std::memcpy(base + fieldsAddress, &out, sizeof(out));
}

// BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS TimeFields, PLARGE_INTEGER Time)
// The inverse, provided alongside because titles that convert one way convert
// back, and a lone forward routine would trap on the return trip.
void __imp__RtlTimeFieldsToTime(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t fieldsAddress = ctx.r3.u32;
    const uint32_t timeAddress = ctx.r4.u32;

    GuestTimeFields in;
    std::memcpy(&in, base + fieldsAddress, sizeof(in));
    const int year = int(ByteSwap(in.year));
    const unsigned month = ByteSwap(in.month);
    const unsigned day = ByteSwap(in.day);
    const unsigned hour = ByteSwap(in.hour);
    const unsigned minute = ByteSwap(in.minute);
    const unsigned second = ByteSwap(in.second);
    const unsigned ms = ByteSwap(in.milliseconds);

    const std::chrono::year_month_day ymd{
        std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59 || ms > 999 || !ymd.ok())
    {
        ctx.r3.u64 = 0; // FALSE: the fields do not describe a valid time
        return;
    }

    const auto dayPart = std::chrono::sys_days{ymd}.time_since_epoch();
    const int64_t ticksUnix =
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
            std::chrono::duration_cast<std::chrono::milliseconds>(dayPart) +
            std::chrono::hours{hour} + std::chrono::minutes{minute} +
            std::chrono::seconds{second} + std::chrono::milliseconds{ms}).count();
    const uint64_t ticks1601 = uint64_t(ticksUnix + kEpochDelta100ns);
    *reinterpret_cast<uint64_t*>(base + timeAddress) = ByteSwap(ticks1601);
    ctx.r3.u64 = 1; // TRUE
}
