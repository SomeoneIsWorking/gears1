// Counted-string helpers and status translation.
#include "import_stub.h"

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
