// printf-family formatting, overridden natively.
//
// These are guest CRT routines, but formatting is a place where reimplementing
// the title's own code buys nothing: the observable behaviour is fully
// specified by C, and the host library already implements it correctly. What
// the override has to get right is not the formatting but the *argument
// reading*, which follows the console's calling convention rather than the
// host's.
//
// Conventions that matter here:
//   - Integer, pointer and string arguments occupy one 4-byte slot each.
//   - 64-bit integers take two slots and start on an even slot boundary.
//   - Floating-point varargs travel in f1..f13, and still consume two integer
//     slots, so skipping those slots is required or every argument after a
//     %f comes out shifted.
//   - sprintf's varargs begin in r5 and continue in r6..r10 before spilling to
//     the caller's parameter area; _vsnprintf is handed a va_list that already
//     points at a homed, contiguous run of slots.
#include "import_stub.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <lucent/log.h>

#include <byteswap.h>

namespace
{
constexpr uint32_t kMaxRegisterArgs = 6;    // r5..r10
constexpr uint32_t kMaxFloatArgs = 13;      // f1..f13

// Reads successive varargs in guest order, from registers then memory for
// sprintf, or straight from memory for a va_list.
class GuestArgs
{
public:
    GuestArgs(PPCContext& ctx, uint8_t* base, uint32_t vaList)
        : ctx_(ctx), base_(base), memory_(vaList), fromRegisters_(vaList == 0) {}

    uint32_t NextU32()
    {
        if (fromRegisters_ && slot_ < kMaxRegisterArgs)
        {
            const PPCRegister* registers[kMaxRegisterArgs] = {
                &ctx_.r5, &ctx_.r6, &ctx_.r7, &ctx_.r8, &ctx_.r9, &ctx_.r10 };
            return registers[slot_++]->u32;
        }

        const uint32_t address = SlotAddress();
        ++slot_;
        return ByteSwap(*reinterpret_cast<uint32_t*>(base_ + address));
    }

    uint64_t NextU64()
    {
        if ((slot_ & 1) != 0)
            ++slot_; // 64-bit values start on an even slot
        const uint64_t high = NextU32();
        const uint64_t low = NextU32();
        return (high << 32) | low;
    }

    // Floating-point varargs are in the FPRs, but still consume integer slots.
    double NextDouble()
    {
        const double value = float_ < kMaxFloatArgs ? (&ctx_.f1)[float_].f64 : 0.0;
        ++float_;
        if ((slot_ & 1) != 0)
            ++slot_;
        slot_ += 2;
        return value;
    }

private:
    uint32_t SlotAddress() const
    {
        if (!fromRegisters_)
            return memory_ + slot_ * 4;
        // Past r10, arguments continue in the caller's parameter area. r3 and
        // r4 own the first two slots, so the varargs spill follows them.
        return ctx_.r1.u32 + 0x08 + (2 + slot_) * 4;
    }

    PPCContext& ctx_;
    uint8_t* base_;
    uint32_t memory_;
    bool fromRegisters_;
    uint32_t slot_ = 0;
    uint32_t float_ = 0;
};

// Formats one conversion with the host library, so the exact semantics of
// width, precision and flags come from an implementation that already has them
// right rather than from a reimplementation of them here.
void AppendConversion(std::string& out, const std::string& spec, char conversion,
    GuestArgs& args, uint8_t* base, bool isLong)
{
    char buffer[512];

    switch (conversion)
    {
    case 'd': case 'i':
        if (isLong)
            snprintf(buffer, sizeof(buffer), spec.c_str(), int64_t(args.NextU64()));
        else
            snprintf(buffer, sizeof(buffer), spec.c_str(), int32_t(args.NextU32()));
        break;

    case 'u': case 'o': case 'x': case 'X':
        if (isLong)
            snprintf(buffer, sizeof(buffer), spec.c_str(), args.NextU64());
        else
            snprintf(buffer, sizeof(buffer), spec.c_str(), args.NextU32());
        break;

    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        snprintf(buffer, sizeof(buffer), spec.c_str(), args.NextDouble());
        break;

    case 'c':
        snprintf(buffer, sizeof(buffer), spec.c_str(), int(args.NextU32()));
        break;

    case 'p':
        // Printed as a guest address; the host pointer would be meaningless in
        // a log the title wrote about its own memory.
        snprintf(buffer, sizeof(buffer), "0x%08X", args.NextU32());
        break;

    case 's':
    {
        const uint32_t address = args.NextU32();
        const char* text = address != 0
            ? reinterpret_cast<const char*>(base + address) : "(null)";
        snprintf(buffer, sizeof(buffer), spec.c_str(), text);
        break;
    }

    default:
        lucent::warn("crt", "unsupported conversion '%{}' in format", conversion);
        snprintf(buffer, sizeof(buffer), "%%%c", conversion);
        break;
    }

    out += buffer;
}

std::string Format(const char* format, GuestArgs& args, uint8_t* base)
{
    std::string out;

    for (const char* p = format; *p != '\0'; ++p)
    {
        if (*p != '%')
        {
            out += *p;
            continue;
        }

        if (*(p + 1) == '%')
        {
            out += '%';
            ++p;
            continue;
        }

        std::string spec("%");
        bool isLong = false;
        ++p;

        // Flags, width and precision pass through untouched; only the length
        // modifiers need interpreting, because they decide how many slots the
        // argument occupies.
        while (*p != '\0' && std::strchr("-+ #0123456789.*", *p) != nullptr)
        {
            if (*p == '*')
            {
                spec += std::to_string(int32_t(args.NextU32()));
                ++p;
                continue;
            }
            spec += *p++;
        }

        while (*p == 'l' || *p == 'h' || *p == 'I' || *p == '6' || *p == '4' || *p == 'z')
        {
            if (*p == 'l' && *(p + 1) == 'l')
                isLong = true;
            if (*p == 'I' && *(p + 1) == '6')
                isLong = true;
            ++p;
        }

        if (*p == '\0')
            break;

        const char conversion = *p;
        if (isLong && std::strchr("diouxX", conversion) != nullptr)
            spec += "ll";
        spec += conversion;

        AppendConversion(out, spec, conversion, args, base, isLong);
    }

    return out;
}

// Writes the result under the count limit and returns what the C library
// would: the length written, or -1 when it did not fit.
int32_t Emit(uint8_t* base, uint32_t buffer, size_t count, const std::string& text)
{
    if (buffer == 0)
        return -1;

    char* destination = reinterpret_cast<char*>(base + buffer);
    if (text.size() >= count)
    {
        std::memcpy(destination, text.data(), count);
        return -1;
    }

    std::memcpy(destination, text.data(), text.size());
    destination[text.size()] = '\0';
    return int32_t(text.size());
}
} // namespace

// int _vsnprintf(char* buffer, size_t count, const char* format, va_list args)
void __imp___vsnprintf(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t buffer = ctx.r3.u32;
    const size_t count = ctx.r4.u32;
    const uint32_t format = ctx.r5.u32;

    if (format == 0)
    {
        ctx.r3.s64 = -1;
        return;
    }

    GuestArgs args(ctx, base, ctx.r6.u32);
    const std::string text = Format(reinterpret_cast<const char*>(base + format), args, base);
    ctx.r3.s64 = Emit(base, buffer, count, text);
}

// int sprintf(char* buffer, const char* format, ...)
void __imp__sprintf(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t format = ctx.r4.u32;

    if (format == 0)
    {
        ctx.r3.s64 = -1;
        return;
    }

    GuestArgs args(ctx, base, 0);
    const std::string text = Format(reinterpret_cast<const char*>(base + format), args, base);

    // sprintf has no bound; the guest promises the buffer is large enough.
    ctx.r3.s64 = Emit(base, buffer, text.size() + 1, text);
}
