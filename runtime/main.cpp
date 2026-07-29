#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <image.h>
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_memory.h"
#include "guest_heap.h"
#include "guest_filesystem.h"
#include "user_profile.h"
#include "guest_thread.h"
#include "fatal_exit.h"

namespace gears { bool CommitDeviceWindow(GuestMemory& memory); }
#include "wait_probe.h"
#include "xma.h"
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

// Proving the detector can fire. A sanitizer that reports nothing and a
// sanitizer that is not actually watching produce the same output -- silence --
// so an ASan build is worth nothing until it has been shown to report a fault
// in THIS process, with its 4 GiB guest reservation mapped and its host heap in
// the state the runtime leaves it. GEARS_ASAN_SELFTEST=1 does exactly one
// out-of-bounds read of a heap allocation and expects a report. It exists only
// in a sanitizer build; a normal build has no such code path at all.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define GEARS_ASAN_BUILD 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define GEARS_ASAN_BUILD 1
#endif

#ifdef GEARS_ASAN_BUILD
namespace
{
void AsanSelfTest()
{
    volatile uint8_t* block = new uint8_t[64]();
    lucent::info("asan", "self-test: reading one byte past a 64-byte heap block");
    // Deliberately out of bounds. volatile so it is not optimised away.
    volatile uint8_t v = block[64];
    lucent::error("asan", "self-test READ {} WITHOUT A REPORT -- the sanitizer is "
                          "not watching this process", int(v));
    delete[] block;
}
} // namespace
#endif

int main(int argc, char* argv[])
{
    lucent::config::set_prefix("GEARS_");

    if (argc < 2)
    {
        lucent::error("boot", "usage: {} <path to default.xex> [game data directory]", argv[0]);
        return EXIT_FAILURE;
    }

    // Where the title's own data files live, extracted from the user's disc.
    // Without it the game runs but every file open fails, which is a legitimate
    // way to test the rest of the runtime.
    const char* gameDirectory = argc >= 3 ? argv[2] : getenv("GEARS_GAME_DIR");
    if (gameDirectory != nullptr)
        gears::Files().SetGameDirectory(gameDirectory);
    else
        lucent::warn("fs", "no game directory given; all file opens will fail");

    // The player's profile settings, from the last run. A first run has none,
    // which is not an error -- the title then reads every setting as unset and
    // uses its own defaults, exactly as a freshly created console profile does.
    gears::Profile().Load(gears::Files().SaveDirectory() / "profile.bin");

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

    gears::SetLoadedImage(image);
    if (!gears::InstallExecutableModule(memory, xex.data(), xex.size()))
        return EXIT_FAILURE;
    gears::ResolveImportVariables(memory, image);
    gears::InitialiseHeaps(memory);

    if (!gears::CommitDeviceWindow(memory))
        return EXIT_FAILURE;

    // Must follow the device window, since it publishes a register into it.
    if (!gears::SetupXmaRegisters(memory))
        return EXIT_FAILURE;

    // Watches for the guest going quiet and reports what every guest thread was
    // doing when it did (catalog #44). Costs one atomic increment per kernel
    // call and a thread that sleeps.
    gears::StartStallDetector();

#ifdef GEARS_ASAN_BUILD
    if (lucent::config::flag("ASAN_SELFTEST"))
        AsanSelfTest();
#endif

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

    // NOT `memory.Release(); return EXIT_SUCCESS;`, which is what stood here.
    //
    // The guest's entry point returning does not mean the process is quiet.
    // Every thread ExCreateThread made is DETACHED (kernel_thread.cpp), and so
    // are the three the runtime drives itself: the tick publisher
    // (guest_thread.cpp, which writes into guest memory every millisecond), the
    // timer scheduler (kernel_timer.cpp) and the stall detector
    // (wait_probe.cpp). Nothing joins any of them. So the old ending did two
    // things underneath about twenty live threads:
    //
    //   - munmap'd the whole guest address space (GuestMemory::Release), after
    //     which the tick publisher's next store, and any guest code still
    //     running, writes to unmapped memory;
    //   - returned from main, which destroys `memory` -- a LOCAL that
    //     gears::Memory() hands out by reference -- and then runs every
    //     function-local and namespace-scope static: the two heaps
    //     (guest_heap.cpp InitialiseHeaps), the critical-section and spin-lock
    //     tables, the handle table, the open-file table.
    //
    // That second half is precisely the failure fatal_exit.h was written for --
    // ASan caught a heap-use-after-free in HostLockFor on the audio pump,
    // freed by ~unordered_map from __run_exit_handlers -- except that file only
    // fixed the KeBugCheck path. This one is the normal way a run ends, so it
    // is the more reachable of the two.
    //
    // There is no correct teardown to write instead: a detached thread executing
    // recompiled guest code cannot be asked to stop, and the console's own
    // behaviour when the entry point returns is that the process ends. So it
    // ends here, the same way, running no destructors.
    gears::FatalExit(EXIT_SUCCESS);
}
