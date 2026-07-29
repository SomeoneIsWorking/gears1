// Console configuration settings.
#include "import_stub.h"

#include <cstring>
#include <optional>
#include <vector>

#include <byteswap.h>
#include <lucent/log.h>

namespace
{

constexpr uint16_t kSecuredCategory = 0x0002;
constexpr uint16_t kUserCategory = 0x0003;

// NOT EVERY SETTING IS FOUR BYTES. The time-zone names are UTF-16 strings, so a
// setting is a byte range with its own length rather than a uint32 -- the
// earlier version of this file assumed 4 everywhere and reported RequiredSize=4
// unconditionally, which is a wrong answer for any setting that is not a DWORD.
struct ConfigSetting
{
    std::vector<uint8_t> bytes;
};

// Big-endian, because the guest reads these as its own words.
ConfigSetting Dword(uint32_t value)
{
    return {{uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8),
             uint8_t(value)}};
}

// Only settings with a defensible value are answered. An unknown one is
// reported by name rather than answered with a zero, because a wrong console
// setting produces misbehaviour far from its cause.
std::optional<ConfigSetting> ConfigValue(uint16_t category, uint16_t setting)
{
    if (category == kUserCategory)
    {
        switch (setting)
        {
        case 0x0001: return Dword(0);          // time zone bias
        case 0x0007: return Dword(1);          // language: English
        case 0x0008: return Dword(0x00040000); // video flags
        case 0x0009: return Dword(0);          // audio flags: stereo
        case 0x000A: return Dword(1);          // retail flags
        case 0x000E: return Dword(103);        // country: US
        default: break;
        }
    }
    else if (category == kSecuredCategory)
    {
        switch (setting)
        {
        case 0x0002: return Dword(0x00001000); // AV region
        default: break;
        }
    }

    return std::nullopt;
}

} // namespace

// NTSTATUS ExGetXConfigSetting(WORD Category, WORD Setting, PVOID Buffer,
//                              WORD BufferSize, PWORD RequiredSize)
void __imp__ExGetXConfigSetting(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint16_t category = uint16_t(ctx.r3.u32);
    const uint16_t setting = uint16_t(ctx.r4.u32);
    const uint32_t buffer = ctx.r5.u32;
    const uint32_t bufferSize = ctx.r6.u32;
    const uint32_t requiredSizePtr = ctx.r7.u32;

    const auto value = ConfigValue(category, setting);
    if (!value)
    {
        // The buffer size is part of the question, not decoration: it says
        // whether the title wants a DWORD or a string, which is what decides
        // how an unimplemented setting has to be answered once it is known.
        lucent::error("config", "unknown XConfig setting: category {:#x} setting"
            " {:#x}, into a {}-byte buffer", category, setting, bufferSize);
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    const uint16_t size = uint16_t(value->bytes.size());
    if (requiredSizePtr != 0)
        *reinterpret_cast<uint16_t*>(base + requiredSizePtr) = ByteSwap(size);

    if (buffer != 0)
    {
        if (bufferSize < size)
        {
            ctx.r3.u64 = 0xC0000023; // STATUS_BUFFER_TOO_SMALL
            return;
        }
        std::memcpy(base + buffer, value->bytes.data(), size);
    }

    lucent::debug("config", "XConfig {:#x}/{:#x} -> {} bytes", category, setting, size);
    ctx.r3.u64 = gears::kStatusSuccess;
}
