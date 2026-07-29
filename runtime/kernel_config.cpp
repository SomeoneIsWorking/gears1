// The guest seam for console configuration settings. The table itself, and the
// setting identifiers, live in xconfig.cpp where they are tested.
#include "import_stub.h"

#include <cstring>

#include <byteswap.h>
#include <lucent/log.h>

#include "xconfig.h"

// NTSTATUS ExGetXConfigSetting(WORD Category, WORD Setting, PVOID Buffer,
//                              WORD BufferSize, PWORD RequiredSize)
void __imp__ExGetXConfigSetting(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint16_t category = uint16_t(ctx.r3.u32);
    const uint16_t setting = uint16_t(ctx.r4.u32);
    const uint32_t buffer = ctx.r5.u32;
    const uint32_t bufferSize = ctx.r6.u32;
    const uint32_t requiredSizePtr = ctx.r7.u32;

    const auto value = gears::ConfigValue(category, setting);
    if (!value)
    {
        // The buffer size is part of the question, not decoration: it says
        // whether the title wants a DWORD or a string, which is what decides
        // how an unimplemented setting has to be answered once it is known.
        lucent::error("config", "XConfig category {:#x} setting {:#x}: {} byte"
            " buffer -> UNKNOWN, refused", category, setting, bufferSize);
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

    lucent::info("config", "XConfig category {:#x} setting {:#x}: {} byte buffer"
        " -> answered with {} bytes", category, setting, bufferSize, size);
    ctx.r3.u64 = gears::kStatusSuccess;
}
