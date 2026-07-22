#include "import_variables.h"

#include <lucent/log.h>

#include "byteswap.h"

namespace gears
{

namespace
{
// Guest-visible storage for imported variables. The Xbox 360 loader would place
// these in the kernel's own data; here they just need a stable guest address the
// game can dereference. Sits above the image and below the stack.
constexpr uint32_t kVariableHeapBase = 0x70100000;
constexpr uint32_t kVariableHeapSize = 0x10000;
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

uint32_t ExecutableModuleHandle()
{
    return uint32_t(PPC_IMAGE_BASE);
}

size_t ResolveImportVariables(GuestMemory& memory, const Image& image)
{
    if (image.importVariables.empty())
        return 0;

    if (!memory.Commit(kVariableHeapBase, kVariableHeapSize))
        return 0;

    const size_t needed = image.importVariables.size() * sizeof(uint32_t);
    if (needed > kVariableHeapSize)
    {
        lucent::error("loader", "{} import variables need {} bytes, heap is {}",
            image.importVariables.size(), needed, kVariableHeapSize);
        return 0;
    }

    size_t unnamed = 0;
    uint32_t cursor = kVariableHeapBase;

    for (const ImportVariable& var : image.importVariables)
    {
        // The thunk slot holds the *address* of the variable; the variable
        // itself starts zeroed. Where a zero is not a valid initial value the
        // game will tell us by misbehaving -- better than inventing a value.
        //
        // The executable's own module handle is the exception: its correct
        // value is known, and XexGetModuleHandle must hand back the same one.
        *memory.Translate<uint32_t>(var.thunkAddress) = ByteSwap(cursor);
        *memory.Translate<uint32_t>(cursor) =
            var.name == "XexExecutableModuleHandle" ? ByteSwap(ExecutableModuleHandle()) : 0;

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

        cursor += sizeof(uint32_t);
    }

    lucent::info("loader", "resolved {} import variables ({} with unknown ordinals)",
        image.importVariables.size(), unnamed);
    return image.importVariables.size();
}

} // namespace gears
