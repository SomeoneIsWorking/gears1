#include "import_variables.h"

#include <cstring>

#include <lucent/log.h>

#include "byteswap.h"
#include "kernel_time.h"

namespace gears
{

namespace
{
// Guest-visible storage for imported variables. The Xbox 360 loader would place
// these in the kernel's own data; here they just need a stable guest address the
// game can dereference. Sits above the image and below the stack.
constexpr uint32_t kVariableHeapBase = 0x70100000;
constexpr uint32_t kVariableHeapSize = 0x10000;

// Import names arrive carrying the thunk prefix ("__imp__KeTimeStampBundle");
// comparisons against kernel export names want it gone.
std::string_view BareName(const std::string& name)
{
    std::string_view view = name;
    constexpr std::string_view kPrefix = "__imp__";
    if (view.substr(0, kPrefix.size()) == kPrefix)
        view.remove_prefix(kPrefix.size());
    return view;
}

// Most imported variables are a single pointer-sized cell, but some name a
// structure the title indexes into; those need their real footprint or the
// fields would overlap the next variable's storage.
uint32_t VariableSize(std::string_view name)
{
    if (name == "KeTimeStampBundle")
        return 0x18; // interrupt time u64, system time u64, tick count u32, pad
    return sizeof(uint32_t);
}
} // namespace


namespace
{
const Image* g_loadedImage = nullptr;
} // namespace

void SetLoadedImage(const Image& image)
{
    g_loadedImage = &image;
}

const Image* LoadedImage()
{
    return g_loadedImage;
}

namespace
{
// Above the import-variable heap (0x70100000 + 0x10000).
constexpr uint32_t kModuleEntryAddress = 0x70110000;
constexpr uint32_t kModuleEntrySize = 0xA0;
constexpr uint32_t kXexHeaderAddress = 0x70110100;
constexpr uint32_t kXexHeaderPointerOffset = 0x58; // titles read *(module+0x58)

uint32_t g_moduleHandle = 0;

uint32_t ReadBe32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
} // namespace

bool InstallExecutableModule(GuestMemory& memory, const uint8_t* xexData, size_t xexSize)
{
    if (xexSize < 0x18 || memcmp(xexData, "XEX2", 4) != 0)
    {
        lucent::error("loader", "executable is not an XEX2 image");
        return false;
    }

    // The header region runs from the start of the file to the PE data.
    const uint32_t headerSize = ReadBe32(xexData + 0x08);
    if (headerSize < 0x18 || headerSize > xexSize)
    {
        lucent::error("loader", "XEX header region size {:#x} is implausible", headerSize);
        return false;
    }

    if (!memory.Commit(kModuleEntryAddress,
            (kXexHeaderAddress - kModuleEntryAddress) + headerSize))
        return false;

    // The module entry: everything a title has been seen to read is the XEX
    // header pointer; the rest stays zero so an unexpected read shows up as a
    // zero rather than as plausible noise.
    memset(memory.Translate<uint8_t>(kModuleEntryAddress), 0, kModuleEntrySize);
    *memory.Translate<uint32_t>(kModuleEntryAddress + kXexHeaderPointerOffset) =
        ByteSwap(kXexHeaderAddress);

    // The header is copied verbatim, so anything that walks it -- the
    // RtlImageXexHeaderField implementation or the title's own parsing --
    // sees the real thing.
    memcpy(memory.Translate<uint8_t>(kXexHeaderAddress), xexData, headerSize);

    g_moduleHandle = kModuleEntryAddress;
    lucent::info("loader", "module entry at {:#x}, XEX header ({} bytes) at {:#x}",
        kModuleEntryAddress, headerSize, kXexHeaderAddress);
    return true;
}

uint32_t ExecutableModuleHandle()
{
    return g_moduleHandle;
}

size_t ResolveImportVariables(GuestMemory& memory, const Image& image)
{
    if (image.importVariables.empty())
        return 0;

    if (!memory.Commit(kVariableHeapBase, kVariableHeapSize))
        return 0;

    size_t unnamed = 0;
    uint32_t cursor = kVariableHeapBase;

    for (const ImportVariable& var : image.importVariables)
    {
        const std::string_view name = BareName(var.name);
        const uint32_t size = VariableSize(name);
        if (size > sizeof(uint32_t))
            cursor = (cursor + 7) & ~7u; // structures hold 64-bit fields
        if (uint64_t(cursor) + size > uint64_t(kVariableHeapBase) + kVariableHeapSize)
        {
            lucent::error("loader", "import variable heap exhausted at {}", var.name);
            return 0;
        }

        // The thunk slot holds the *address* of the variable; the variable
        // itself starts zeroed. Where a zero is not a valid initial value the
        // game will tell us by misbehaving -- better than inventing a value.
        //
        // Two exceptions, where the correct behaviour is known:
        //  - XexExecutableModuleHandle: XexGetModuleHandle must hand back the
        //    same value, so it is filled with the module handle.
        //  - KeTimeStampBundle: on the console the kernel updates this
        //    variable continuously (interrupt time, system time, tick count);
        //    a frozen zero is a clock that never advances, and the title's
        //    Bink movie pacing reads the millisecond field directly.
        // Note: compared without the "__imp__" prefix -- the prefixed
        // comparison silently never matched, which left the module-handle
        // variable zero and the timestamp bundle frozen.
        *memory.Translate<uint32_t>(var.thunkAddress) = ByteSwap(cursor);
        *memory.Translate<uint32_t>(cursor) =
            name == "XexExecutableModuleHandle" ? ByteSwap(ExecutableModuleHandle()) : 0;

        if (name == "KeTimeStampBundle")
            StartKeTimeStampBundle(memory, cursor);

        if (var.name.empty())
        {
            ++unnamed;
            lucent::warn("loader", "unnamed import variable {}:{} -> {:#x}",
                var.library, var.ordinal, cursor);
        }
        else
        {
            lucent::debug("loader", "{} ({}:{}) -> {:#x}",
                var.name, var.library, var.ordinal, cursor);
        }

        cursor += size;
    }

    lucent::info("loader", "resolved {} import variables ({} with unknown ordinals)",
        image.importVariables.size(), unnamed);
    return image.importVariables.size();
}

} // namespace gears
