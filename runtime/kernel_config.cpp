// Console configuration settings.
#include "import_stub.h"

#include <optional>

#include <byteswap.h>
#include <lucent/log.h>

namespace
{

constexpr uint16_t kSecuredCategory = 0x0002;
constexpr uint16_t kUserCategory = 0x0003;

// Only settings with a defensible value are answered. An unknown one is
// reported by name rather than answered with a zero, because a wrong console
// setting produces misbehaviour far from its cause.
std::optional<uint32_t> ConfigValue(uint16_t category, uint16_t setting)
{
    if (category == kUserCategory)
    {
        switch (setting)
        {
        case 0x0001: return 0;          // time zone bias
        case 0x0007: return 1;          // language: English
        case 0x0008: return 0x00040000; // video flags
        case 0x0009: return 0;          // audio flags: stereo
        case 0x000A: return 1;          // retail flags
        case 0x000E: return 103;        // country: US
        default: break;
        }
    }
    else if (category == kSecuredCategory)
    {
        switch (setting)
        {
        case 0x0002: return 0x00001000; // AV region
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
        lucent::error("config", "unknown XConfig setting: category {:#x} setting {:#x}",
            category, setting);
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    if (requiredSizePtr != 0)
        *reinterpret_cast<uint16_t*>(base + requiredSizePtr) = ByteSwap(uint16_t(4));

    if (buffer != 0)
    {
        if (bufferSize < 4)
        {
            ctx.r3.u64 = 0xC0000023; // STATUS_BUFFER_TOO_SMALL
            return;
        }
        *reinterpret_cast<uint32_t*>(base + buffer) = ByteSwap(*value);
    }

    lucent::debug("config", "XConfig {:#x}/{:#x} -> {:#x}", category, setting, *value);
    ctx.r3.u64 = gears::kStatusSuccess;
}
