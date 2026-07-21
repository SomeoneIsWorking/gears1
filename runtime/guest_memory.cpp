#include "guest_memory.h"

#include <sys/mman.h>

#include <lucent/log.h>

namespace gears
{

namespace
{
// PPC_LOOKUP_FUNC indexes a table at base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE,
// scaling (address - PPC_CODE_BASE) by 2. Guest instructions are 4-byte
// aligned, so that scaling yields 8 bytes -- one host pointer -- per
// instruction slot.
constexpr uint64_t kFuncTableBase = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
constexpr uint64_t kFuncTableSize = uint64_t(PPC_CODE_SIZE) * 2;
} // namespace

bool GuestMemory::Reserve()
{
    reservedSize_ = kFuncTableBase + kFuncTableSize;
    static_assert(kFuncTableBase + kFuncTableSize <= PPC_MEMORY_SIZE,
        "function table must fit inside the 4 GiB guest window");

    // Reserved without backing pages; Commit() faults in only what is used.
    // MAP_NORESERVE keeps this off the commit charge on overcommit-strict hosts.
    void* p = mmap(nullptr, reservedSize_, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
    {
        lucent::error("mem", "failed to reserve {} bytes of guest address space", reservedSize_);
        return false;
    }

    base_ = static_cast<uint8_t*>(p);

    // PPC_FUNC_PROLOGUE tells Clang to assume this; an unaligned base would
    // silently miscompile every guest function.
    if ((reinterpret_cast<uintptr_t>(base_) & 0x1F) != 0)
    {
        lucent::error("mem", "guest base {} is not 32-byte aligned", static_cast<void*>(base_));
        Release();
        return false;
    }

    lucent::info("mem", "reserved {:.2f} GiB of guest address space at {}",
        double(reservedSize_) / (1024.0 * 1024.0 * 1024.0), static_cast<void*>(base_));
    return true;
}

void GuestMemory::Release()
{
    if (base_ != nullptr)
    {
        munmap(base_, reservedSize_);
        base_ = nullptr;
    }
}

bool GuestMemory::Commit(uint32_t address, uint32_t size)
{
    if (size == 0)
        return true;

    // mprotect needs page-aligned bounds, so widen to the enclosing pages.
    const uintptr_t pageSize = 0x1000;
    uintptr_t begin = uintptr_t(address) & ~(pageSize - 1);
    uintptr_t end = (uintptr_t(address) + size + pageSize - 1) & ~(pageSize - 1);

    if (mprotect(base_ + begin, end - begin, PROT_READ | PROT_WRITE) != 0)
    {
        lucent::error("mem", "failed to commit guest range {:#x}..{:#x}", begin, end);
        return false;
    }

    lucent::debug("mem", "committed {:#x}..{:#x} ({} KiB)", begin, end, (end - begin) / 1024);
    return true;
}

namespace
{
GuestMemory* g_memory = nullptr;
}

GuestMemory& Memory()
{
    return *g_memory;
}

void SetMemory(GuestMemory& memory)
{
    g_memory = &memory;
}

size_t InstallFunctionTable(GuestMemory& memory)
{
    if (!memory.Commit(uint32_t(kFuncTableBase), uint32_t(kFuncTableSize)))
        return 0;

    uint8_t* base = memory.Base();
    size_t installed = 0;
    size_t outOfRange = 0;

    for (const PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; ++m)
    {
        // Imports live outside the .text range the table covers; they are
        // called directly by name from the recompiled code, not through it.
        if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
        {
            ++outOfRange;
            continue;
        }

        PPC_LOOKUP_FUNC(base, m->guest) = m->host;
        ++installed;
    }

    lucent::info("loader", "installed {} functions into the indirect-call table ({} outside .text)",
        installed, outOfRange);
    return installed;
}

} // namespace gears
