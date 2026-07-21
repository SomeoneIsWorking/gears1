#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <image.h>
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_memory.h"
#include "guest_heap.h"
#include "guest_thread.h"
#include "import_variables.h"
#include "ppc_recomp_shared.h"

PPC_EXTERN_FUNC(_xstart);

namespace
{

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::vector<uint8_t> data;
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return data;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    data.resize(size_t(size));
    if (fread(data.data(), 1, data.size(), f) != data.size())
        data.clear();

    fclose(f);
    return data;
}

// The guest stack, allocated from the title heap with the rest of the thread
// block. The Xbox 360 ABI grows it downwards from the high address.
constexpr uint32_t kStackSize = 1 * 1024 * 1024;

} // namespace

int main(int argc, char* argv[])
{
    lucent::config::set_prefix("GEARS_");

    if (argc < 2)
    {
        lucent::error("boot", "usage: {} <path to default.xex>", argv[0]);
        return EXIT_FAILURE;
    }

    const std::filesystem::path xexPath = argv[1];
    std::vector<uint8_t> xex = ReadFile(xexPath);
    if (xex.empty())
    {
        lucent::error("boot", "cannot read {}", xexPath.string());
        return EXIT_FAILURE;
    }
    lucent::info("boot", "read {} ({} bytes)", xexPath.string(), xex.size());

    Image image = Image::ParseImage(xex.data(), xex.size());
    lucent::info("boot", "image base {:#x}, size {:#x}, entry {:#x}",
        image.base, image.size, image.entry_point);

    // The recompiled code was generated against one specific image layout; if
    // the runtime is handed a different build, every address is wrong.
    if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
    {
        lucent::error("boot",
            "image layout {:#x}/{:#x} does not match the recompiled code's {:#x}/{:#x} "
            "-- this XEX is not the one the C++ was generated from",
            image.base, image.size, uint64_t(PPC_IMAGE_BASE), uint64_t(PPC_IMAGE_SIZE));
        return EXIT_FAILURE;
    }

    gears::GuestMemory memory;
    if (!memory.Reserve())
        return EXIT_FAILURE;

    gears::SetMemory(memory);

    if (!memory.Commit(uint32_t(image.base), image.size))
        return EXIT_FAILURE;
    memcpy(memory.Base() + image.base, image.data.get(), image.size);
    lucent::info("loader", "mapped image into guest memory");

    if (gears::InstallFunctionTable(memory) == 0)
        return EXIT_FAILURE;

    gears::ResolveImportVariables(memory, image);
    gears::InitialiseHeaps(memory);

    gears::GuestThreadBlock mainThread{};
    if (!gears::CreateGuestThreadBlock(memory, kStackSize, mainThread))
        return EXIT_FAILURE;

    PPCContext ctx{};
    // r13 is the thread pointer; guest code reaches its thread block through it.
    ctx.r13.u32 = mainThread.pcrAddress;
    ctx.r1.u32 = mainThread.stackBase - 0x100;
    ctx.fpscr.loadFromHost();

    lucent::info("boot", "entering guest at {:#x} with r1={:#x}", image.entry_point, ctx.r1.u32);
    _xstart(ctx, memory.Base());
    lucent::info("boot", "guest entry point returned");

    memory.Release();
    return EXIT_SUCCESS;
}
