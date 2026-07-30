// Tests for the fault report's address classifier.
//
// WHY THIS EXISTS AT ALL. Six consecutive runs of the crash repro produced a
// core dump and NOT ONE LINE about where the fault was. Every conclusion drawn
// from those runs about "the crashing holder" came from an earlier build that
// still had a reporter; the runs that were supposed to be testing hypotheses
// were, at the fault itself, blind. A crash that prints nothing is the purest
// form of the diagnostic-that-can-print-nothing problem: the process simply
// stops, and whatever you believed before the run survives untouched.
//
// The handler itself cannot be unit-tested meaningfully -- it runs on a signal
// stack in a process that is already broken. What CAN be tested, and is the
// part that actually decides whether the report says anything useful, is the
// classification of the faulting address: guest memory and where in it, the
// null page that the guest maps as real physical RAM, or a host pointer that
// has nothing to do with the guest at all.
//
// That distinction is not cosmetic here. This runtime maps guest physical
// address 0 as a real window, so a guest null dereference reads live RAM
// instead of faulting -- which means a fault at a low address is NOT a guest
// null pointer and must not be reported as one.

#include <cstdio>
#include <cstdint>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "fault_report.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// A pretend guest mapping. The real one is 4 GiB at some host base; the
// classifier only needs base and size, so the test can use a small one.
constexpr uintptr_t kBase = 0x100000000ull;
constexpr uint64_t kSize = 0x20000000ull;   // 512 MiB

void TestAddressInsideGuestMemory()
{
    const std::string s =
        gears::DescribeFaultAddress(kBase, kSize, kBase + 0x82000000ull % kSize);
    Check(Contains(s, "guest"),
        "inside: an address in the mapping is named as guest memory");
    Check(Contains(s, "0x"),
        "inside: and the report carries the guest address it corresponds to");
}

// The distinction the mapping makes necessary: a fault at a low guest offset is
// a fault in the PHYSICAL WINDOW, not a null pointer, because this runtime maps
// address zero as real memory.
void TestLowGuestAddressIsNotCalledNull()
{
    const std::string s = gears::DescribeFaultAddress(kBase, kSize, kBase + 0x40);
    Check(Contains(s, "guest"),
        "low: an address just above the base is still guest memory");
    Check(!Contains(s, "null pointer"),
        "low: and is NOT reported as a null dereference -- this runtime maps"
        " guest physical zero as real RAM, so a guest null does not fault");
}

// A host address outside the mapping must be said to be outside it, because the
// two have completely different causes: one is the guest misbehaving and the
// other is the runtime itself.
void TestHostAddressOutsideTheMapping()
{
    const std::string below = gears::DescribeFaultAddress(kBase, kSize, 0x1234);
    Check(Contains(below, "host"),
        "outside: an address below the mapping is named as a host pointer");
    Check(!Contains(below, "guest memory"),
        "outside: and is not confused with guest memory");

    const std::string above =
        gears::DescribeFaultAddress(kBase, kSize, kBase + kSize + 0x1000);
    Check(Contains(above, "host"),
        "outside: an address past the end of the mapping is a host pointer too");
}

// The boundaries themselves, where an off-by-one puts a real guest fault in the
// host bucket and sends the next session looking in the runtime for a bug that
// is in the title.
void TestBoundariesAreExact()
{
    Check(Contains(gears::DescribeFaultAddress(kBase, kSize, kBase), "guest"),
        "boundary: the first byte of the mapping is guest memory");
    Check(Contains(
        gears::DescribeFaultAddress(kBase, kSize, kBase + kSize - 1), "guest"),
        "boundary: the last byte of the mapping is guest memory");
    Check(Contains(
        gears::DescribeFaultAddress(kBase, kSize, kBase + kSize), "host"),
        "boundary: one past the end is not");
}

// A null host pointer is its own case and worth saying plainly, since it means
// the runtime dereferenced something it never checked rather than the guest
// going astray.
void TestNullHostPointer()
{
    const std::string s = gears::DescribeFaultAddress(kBase, kSize, 0);
    Check(Contains(s, "host") && Contains(s, "null"),
        "null: address zero is reported as a HOST null dereference, which is a"
        " runtime bug rather than a guest one");
}

// AND DOES THE HANDLER ACTUALLY PRODUCE OUTPUT? Everything above tests the
// string the report is built from, which is necessary and not sufficient: the
// first attempt at this reporter passed every classifier test and printed
// nothing at all when the process faulted, which is the exact failure it was
// written to end.
//
// So: fork, install the reporter in the child, fault on purpose, and have the
// PARENT assert that the report reached the pipe. A crash reporter is only ever
// exercised by crashes, so without this the first real evidence that it works
// arrives at the moment it is needed and not before.
void TestTheHandlerActuallyReports()
{
    int pipes[2];
    if (pipe(pipes) != 0)
    {
        Check(false, "handler: could not create a pipe -- test could not run");
        return;
    }

    const pid_t child = fork();
    if (child == 0)
    {
        // The report goes to stderr, so point stderr at the pipe.
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        gears::SetFaultReportGuestMapping(
            reinterpret_cast<void*>(kBase), kSize);
        gears::InstallFaultReporter();
        volatile uint8_t* deliberate = reinterpret_cast<uint8_t*>(0x10);
        *deliberate = 1;
        _exit(0);               // must not be reached
    }

    close(pipes[1]);
    std::string captured;
    char buffer[512];
    ssize_t n;
    while ((n = read(pipes[0], buffer, sizeof(buffer))) > 0)
        captured.append(buffer, size_t(n));
    close(pipes[0]);

    int status = 0;
    waitpid(child, &status, 0);

    Check(!captured.empty(),
        "handler: the faulting child printed SOMETHING -- an empty capture means"
        " the reporter is dead and every crash will be silent");
    Check(Contains(captured, "=== FAULT ==="),
        "handler: and the report header reached stderr");
    Check(Contains(captured, "SIGSEGV"),
        "handler: naming the signal");
    Check(Contains(captured, "host"),
        "handler: and classifying the address, which for 0x10 is a host pointer");
    Check(WIFSIGNALED(status),
        "handler: the child still died from the signal, so chaining left the"
        " exit status and the core dump alone");

    if (captured.empty())
        printf("  (captured nothing at all)\n");
    else
        printf("  captured %zu bytes of report\n", captured.size());
}

} // namespace

int main()
{
    TestAddressInsideGuestMemory();
    TestLowGuestAddressIsNotCalledNull();
    TestHostAddressOutsideTheMapping();
    TestBoundariesAreExact();
    TestNullHostPointer();
    TestTheHandlerActuallyReports();

    if (g_failures == 0)
    {
        printf("all fault report tests passed\n");
        return 0;
    }
    printf("%d fault report test(s) FAILED\n", g_failures);
    return 1;
}
