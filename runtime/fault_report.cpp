#include "fault_report.h"

#include <cinttypes>
#include <cstdio>
#include <atomic>
#include <cstring>

#include <lucent/config.h>
#include <lucent/log.h>

#ifndef _WIN32
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

namespace gears
{

std::string DescribeFaultAddress(uintptr_t guestBase, uint64_t guestSize,
                                 uintptr_t faultAddress)
{
    char buffer[256];

    if (faultAddress >= guestBase && faultAddress < guestBase + guestSize)
    {
        const uint64_t guestAddress = uint64_t(faultAddress - guestBase);
        // Deliberately NOT calling a low address a null dereference: guest
        // physical zero is a mapped window in this runtime, so a guest null
        // reads real memory and never reaches here. A fault low in the mapping
        // means the mapping itself is not backed there, which is a different
        // and much rarer thing.
        std::snprintf(buffer, sizeof(buffer),
            "guest memory at %#" PRIx64 " (host %p)", guestAddress,
            reinterpret_cast<void*>(faultAddress));
        return buffer;
    }

    if (faultAddress < 0x1000)
    {
        std::snprintf(buffer, sizeof(buffer),
            "a host null dereference (%p) -- this is the runtime dereferencing"
            " something it did not check, not the guest going astray",
            reinterpret_cast<void*>(faultAddress));
        return buffer;
    }

    std::snprintf(buffer, sizeof(buffer),
        "a host pointer outside the guest mapping (%p)",
        reinterpret_cast<void*>(faultAddress));
    return buffer;
}

uintptr_t g_guestBase = 0;
uint64_t g_guestSize = 0;

#ifndef _WIN32
namespace
{

struct sigaction g_previousSegv{};
struct sigaction g_previousBus{};
struct sigaction g_previousAbort{};
volatile sig_atomic_t g_reporting = 0;

// Written with write(2) rather than the logger: the process is already broken,
// and a logger that takes a mutex can deadlock against the thread that faulted
// while holding it. A report that hangs is worse than no report.
void Emit(const char* text)
{
    const ssize_t ignored = write(STDERR_FILENO, text, std::strlen(text));
    (void)ignored;
}

void OnFatalMemorySignal(int signal, siginfo_t* info, void* context)
{
    // A fault inside the reporter itself must not loop forever. One report,
    // then straight to the previous handler.
    if (g_reporting == 0)
    {
        g_reporting = 1;

        Emit("\n=== FAULT ===\n");
        Emit(signal == SIGBUS    ? "signal: SIGBUS\n"
             : signal == SIGABRT ? "signal: SIGABRT (a deliberate stop -- the"
                                   " report above it names the cause)\n"
                                 : "signal: SIGSEGV\n");

        const std::string where = DescribeFaultAddress(
            g_guestBase, g_guestSize,
            reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
        Emit("address: ");
        Emit(where.c_str());
        Emit("\n");

        // THE HOST BACKTRACE IS THE GUEST CALL CHAIN. Recompiled guest
        // functions are ordinary host functions named `sub_82xxxxxx`, so the
        // frames below name guest code directly. backtrace_symbols_fd is used
        // rather than backtrace_symbols because the latter allocates, and
        // allocating from a signal handler in a broken process is how a crash
        // reporter becomes a hang.
        void* frames[64];
        const int count = backtrace(frames, 64);
        Emit("host backtrace (recompiled frames are named for their guest"
             " addresses):\n");
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        Emit("=== END FAULT ===\n");
    }

    // Chain, so a core is still produced and anything else that wanted this
    // signal still sees it.
    const struct sigaction& previous =
        signal == SIGBUS    ? g_previousBus
        : signal == SIGABRT ? g_previousAbort
                            : g_previousSegv;
    if (previous.sa_flags & SA_SIGINFO)
    {
        if (previous.sa_sigaction != nullptr)
        {
            previous.sa_sigaction(signal, info, context);
            return;
        }
    }
    else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN &&
             previous.sa_handler != nullptr)
    {
        previous.sa_handler(signal);
        return;
    }

    // Default: restore and re-raise so the exit status and core are unchanged.
    struct sigaction restore{};
    restore.sa_handler = SIG_DFL;
    sigemptyset(&restore.sa_mask);
    sigaction(signal, &restore, nullptr);
    raise(signal);
}

} // namespace
#endif

// Per-thread alternate stack. Not freed: it must outlive every fault, and a
// process that is exiting has no use for the reclaimed pages.
// IS OUR HANDLER STILL THE ONE INSTALLED? It was installed at the top of main
// and it does not fire when the title crashes, while the identical code fires
// in a forked test -- so something in this process replaces it later. Anything
// that calls sigaction(SIGSEGV) after us wins silently, and a reporter that has
// been quietly displaced is worse than none: it reads as "installed" in the
// source and prints nothing in the run.
void VerifyFaultReporterStillInstalled()
{
#ifndef _WIN32
    static std::atomic<bool> reported{false};
    if (reported.load(std::memory_order_relaxed))
        return;

    struct sigaction current{};
    if (sigaction(SIGSEGV, nullptr, &current) != 0)
        return;

    const bool ours = (current.sa_flags & SA_SIGINFO) &&
                      current.sa_sigaction == &OnFatalMemorySignal;
    if (ours)
        return;

    reported.store(true, std::memory_order_relaxed);
    lucent::error("fault", "THE FAULT REPORTER HAS BEEN DISPLACED: SIGSEGV now"
        " goes to {} (flags {:#x}), not to this runtime's handler. Every crash"
        " from here on will be silent, and whoever installed that handler is the"
        " reason six runs of the repro said nothing about their own fault",
        (current.sa_flags & SA_SIGINFO)
            ? static_cast<void*>(reinterpret_cast<void*>(current.sa_sigaction))
            : static_cast<void*>(reinterpret_cast<void*>(current.sa_handler)),
        uint32_t(current.sa_flags));
#endif
}

void InstallSignalStackForThisThread()
{
#ifndef _WIN32
    static thread_local bool installed = false;
    if (installed)
        return;
    installed = true;

    stack_t stack{};
    stack.ss_size = size_t(SIGSTKSZ) * 4;   // backtrace_symbols_fd needs room
    stack.ss_sp = new uint8_t[stack.ss_size];
    stack.ss_flags = 0;
    sigaltstack(&stack, nullptr);
#endif
}

void SetFaultReportGuestMapping(void* guestBase, uint64_t guestSize)
{
    g_guestBase = reinterpret_cast<uintptr_t>(guestBase);
    g_guestSize = guestSize;

    // PRINTED, because every "that was a host pointer" verdict this reporter
    // gives is only as good as these two numbers. A zero size would make the
    // classifier call EVERY fault a host pointer, which is a confident wrong
    // answer rather than a missing one.
    lucent::info("fault", "fault reporter knows the guest mapping: {} bytes at"
        " {} (so guest {:#x} is host {})", guestSize, guestBase, 0x82000000u,
        static_cast<void*>(static_cast<uint8_t*>(guestBase) + 0x82000000u));
}

void InstallFaultReporter()
{
#ifndef _WIN32

    struct sigaction sa{};
    sa.sa_sigaction = &OnFatalMemorySignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_previousSegv);
    sigaction(SIGBUS, &sa, &g_previousBus);
    // SIGABRT as well: a checked indirect call reports the bad guest address and
    // then aborts, and the host backtrace belongs beside that report.
    sigaction(SIGABRT, &sa, &g_previousAbort);

    // AN ALTERNATE SIGNAL STACK, because the fault this reporter most needs to
    // survive is the one it cannot otherwise report. If the guest overflows the
    // host stack, the handler has nowhere to push its frame, faults again
    // immediately, and the process dies with the default action having printed
    // NOTHING -- which is exactly the silence this file exists to end. The
    // altstack is per-thread, so this covers the main thread; guest threads
    // install their own.
    InstallSignalStackForThisThread();

    // PROVE IT FIRES, IN THE SHIPPING BINARY. A crash reporter is only ever
    // exercised by crashes, so a broken one is discovered at the exact moment it
    // is needed and not before. GEARS_FAULT_SELFTEST=1 makes the process fault
    // on purpose: the report must appear, and if it does not the reporter is
    // broken regardless of how correct it looks.
    if (lucent::config::flag("FAULT_SELFTEST"))
    {
        lucent::info("fault", "selftest: faulting on purpose -- a report MUST"
            " follow this line, and its absence means the reporter is dead");
        volatile uint8_t* deliberate = reinterpret_cast<uint8_t*>(0x10);
        *deliberate = 1;
    }
#endif
}

} // namespace gears
