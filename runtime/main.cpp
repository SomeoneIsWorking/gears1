#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

#include "fault_report.h"
#include "generated_title_profile.h"
#include "guest_clock.h"
#include "guest_memory.h"
#include "guest_heap.h"
#include "guest_filesystem.h"
#include "user_profile.h"
#include "guest_thread.h"
#include "fatal_exit.h"
#include "title_executable.h"

namespace gears
{
bool CommitDeviceWindow(GuestMemory &memory);
}
#include "wait_probe.h"
#include "xma.h"
#include "import_variables.h"
#include "ppc_recomp_shared.h"

PPC_EXTERN_FUNC(_xstart);

namespace
{

std::vector<uint8_t> ReadFile(const std::filesystem::path &path)
{
    std::vector<uint8_t> data;
    FILE *f = fopen(path.c_str(), "rb");
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
#if __has_feature(address_sanitizer)
#define GEARS_ASAN_BUILD 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define GEARS_ASAN_BUILD 1
#endif

#ifdef GEARS_ASAN_BUILD
namespace
{
void AsanSelfTest()
{
    volatile uint8_t *block = new uint8_t[64]();
    lucent::info("asan", "self-test: reading one byte past a 64-byte heap block");
    // Deliberately out of bounds. volatile so it is not optimised away.
    volatile uint8_t v = block[64];
    lucent::error("asan",
                  "self-test READ {} WITHOUT A REPORT -- the sanitizer is "
                  "not watching this process",
                  int(v));
    delete[] block;
}
} // namespace
#endif

int main(int argc, char *argv[])
{
    lucent::config::set_prefix("GEARS_");

    // BEFORE ANYTHING ELSE. Six runs of the crash repro dumped core and said
    // nothing about the fault, so every hypothesis they were meant to test was
    // judged against a log that merely stopped. The reporter chains to the
    // previous disposition, so the core and the exit status are unchanged.
    gears::InstallFaultReporter();

    // NAME THE MAIN THREAD. It runs guest code like any other, but it is the
    // one thread nothing ever named, so it reported as "host" -- the default
    // for a thread that has never run guest code, which is the opposite of what
    // it is. Catalog #44 spent its analysis on a mysterious non-guest thread
    // entering the guest's render-ring producer; the producer's entrant is this
    // thread, and the entry's own next question ("which host thread, and is it
    // a guest callback running on one of ours") had a misleading premise handed
    // to it by a missing label.
    gears::SetGuestThreadName("main");

    // Before the guest runs, because the first mftb may be in static init of
    // the title's own runtime and a clock that changes mode mid-run would go
    // backwards. Reports which mode it chose either way.
    gears::InitGuestClock();

    if (argc < 2)
    {
        lucent::error("boot", "usage: {} <path to default.xex> [game data directory]", argv[0]);
        return EXIT_FAILURE;
    }

    const std::filesystem::path xexPath = argv[1];
    std::vector<uint8_t> xex = ReadFile(xexPath);
    if (xex.empty())
    {
        lucent::error("boot", "cannot read {}", xexPath.string());
        return EXIT_FAILURE;
    }
    // WHICH BUILD IS THIS. Screenshots and logs from a run get compared against
    // fixes that landed at a known time, and "did that binary contain the fix" was
    // guessed at twice. It is one line and it removes the guess.
    lucent::info("boot", "gears1 built {} {} from {}", __DATE__, __TIME__, GEARS_BUILD_REVISION);
    lucent::info("boot", "read {} ({} bytes)", xexPath.string(), xex.size());

    gears::LoadedTitleExecutable loaded;
    std::string loadError;
    if (!gears::LoadTitleExecutable(xex, loaded, loadError))
    {
        lucent::error("boot", "cannot load {}: {}", xexPath.string(), loadError);
        return EXIT_FAILURE;
    }
    Image &image = loaded.image;
    lucent::info("boot", "image base {:#x}, size {:#x}, entry {:#x}", image.base, image.size,
                 image.entry_point);

    // Exact revision selection precedes save state, image mapping, generated
    // function tables, and every title-specific binding. Matching only layout
    // is unsafe because distinct revisions routinely reuse the same addresses.
    const gears::TitleProfileResolution title =
        gears::ResolveTitleProfile(gears::GeneratedTitleProfiles(), loaded.identity);
    if (!title)
    {
        lucent::error("boot", "title profile refusal: {}",
                      gears::TitleProfileErrorText(title.error));
        return EXIT_FAILURE;
    }
    lucent::info("boot", "selected title {}/{}", title.profile->titleKey,
                 title.profile->revisionKey);

    // Where the title's own data files live, extracted from the user's disc.
    // Without it the game runs but every file open fails, which is a legitimate
    // way to test the rest of the runtime.
    const char *gameDirectory = argc >= 3 ? argv[2] : getenv("GEARS_GAME_DIR");
    if (gameDirectory != nullptr)
        gears::Files().SetGameDirectory(gameDirectory);
    else
        lucent::warn("fs", "no game directory given; all file opens will fail");

    if (!gears::Files().SetSaveNamespace(title.profile->saveNamespace))
        return EXIT_FAILURE;

    // The player's profile settings, from the last run. A first run has none,
    // which is not an error -- the title then reads every setting as unset and
    // uses its own defaults, exactly as a freshly created console profile does.
    gears::Profile().Load(gears::Files().SaveDirectory() / "profile.bin");

    gears::GuestMemory memory;
    if (!memory.Reserve())
        return EXIT_FAILURE;

    gears::SetMemory(memory);
    // Now the reporter can tell a guest address from a host pointer. It has been
    // armed since the first line of main; this only sharpens what it prints.
    gears::SetFaultReportGuestMapping(memory.Base(), memory.ReservedSize());

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
