#pragma once

#include <cstdint>

#include "ppc_config.h"
#include "ppc_context.h"

namespace gears
{

// The recompiled code addresses guest memory as `base + guestAddress`, where
// guestAddress is a full 32-bit Xbox 360 address. So `base` has to anchor a
// window covering the entire 4 GiB guest space, and the indirect-call table
// that PPC_LOOKUP_FUNC indexes lives inside it, immediately after the image.
class GuestMemory
{
public:
    bool Reserve();
    void Release();

    uint8_t* Base() const { return base_; }

    // Backs a guest address range with committed, zeroed pages.
    bool Commit(uint32_t address, uint32_t size);

    template<typename T>
    T* Translate(uint32_t address) const
    {
        return reinterpret_cast<T*>(base_ + address);
    }

private:
    uint8_t* base_{};
    size_t reservedSize_{};
};

// Populates the function table PPC_LOOKUP_FUNC reads, from PPCFuncMappings.
// Returns the number of entries installed.
size_t InstallFunctionTable(GuestMemory& memory);

} // namespace gears
