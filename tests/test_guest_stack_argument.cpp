// Tests for reading guest arguments passed on the stack.
//
// This exists because of one wrong register. XamContentCreateEx takes NINE
// parameters and our implementation documented seven, reading the overlapped
// block out of r9 -- which is actually cacheSize, and which the title's own
// wrapper zeroes after spilling the real overlapped to the stack. We therefore
// saw no overlapped, completed the call synchronously and returned success; the
// title's checkpoint loader requires ERROR_IO_PENDING to proceed, so it concluded
// there was nothing to load and reported success having loaded nothing. The whole
// of issue #45 -- an empty archive, a map name of NAME_None, a request for a
// package called "None", and a use-after-free in the title's own error path --
// follows from that.
//
// So the test that matters is not "can we read a stack slot" but "do we read the
// ninth argument rather than a register that happens to be nearby". The decoy
// below is the point.

#include <cstdio>
#include <cstring>
#include <vector>

#include "guest_stack_argument.h"

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

void StoreBE32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

// The offsets are the ABI's, and they are the thing most likely to be silently
// wrong. The title's wrapper spills the ninth argument with `stw r9,84(r1)`, so
// index 8 MUST come out as 84 -- that instruction is the ground truth.
void TestTheNinthArgumentIsAtEightyFour()
{
    Check(gears::GuestStackArgumentOffset(8) == 84,
        "the ninth argument (index 8) is at r1+84 -- the title's own wrapper"
        " spills it there with `stw r9,84(r1)`, which is the ground truth");
    Check(gears::GuestStackArgumentOffset(9) == 92,
        "and the tenth follows eight bytes later");
    Check(gears::GuestStackArgumentOffset(8) != 52,
        "not r1+52, which is what four-byte slots would give -- an easy and"
        " invisible mistake");
}

void TestReadsTheValueAtTheSlot()
{
    std::vector<uint8_t> memory(0x10000, 0xCD);
    const uint32_t stackPointer = 0x1000;

    StoreBE32(memory.data() + stackPointer + 84, 0x40101F70);
    Check(gears::GuestStackArgument32(memory.data(), stackPointer, 8) ==
              0x40101F70,
        "the ninth argument reads back exactly, big-endian");

    StoreBE32(memory.data() + stackPointer + 92, 0xDEADBEEF);
    Check(gears::GuestStackArgument32(memory.data(), stackPointer, 9) ==
              0xDEADBEEF,
        "and so does the tenth, independently of the ninth");
}

// THE DECOY. A neighbouring slot holding a plausible-looking pointer must not be
// picked up: reading the wrong slot is exactly the bug this file commemorates,
// and it produced a value that looked entirely reasonable (zero).
void TestNeighbouringSlotsAreNotConfused()
{
    std::vector<uint8_t> memory(0x10000, 0);
    const uint32_t stackPointer = 0x2000;

    StoreBE32(memory.data() + stackPointer + 76, 0x11111111);  // the eighth
    StoreBE32(memory.data() + stackPointer + 84, 0x22222222);  // the ninth
    StoreBE32(memory.data() + stackPointer + 88, 0x33333333);  // mid-slot
    StoreBE32(memory.data() + stackPointer + 92, 0x44444444);  // the tenth

    Check(gears::GuestStackArgument32(memory.data(), stackPointer, 8) ==
              0x22222222,
        "the ninth argument is the ninth, not the slot before it");
    Check(gears::GuestStackArgument32(memory.data(), stackPointer, 9) ==
              0x44444444,
        "and the tenth is not the second half of the ninth's slot");
}

// Zero is a legitimate value and must be readable as such: an absent overlapped
// really does mean synchronous completion, and the caller has to be able to tell
// that from a misread.
void TestZeroIsAValueNotAFailure()
{
    std::vector<uint8_t> memory(0x10000, 0xFF);
    const uint32_t stackPointer = 0x3000;
    StoreBE32(memory.data() + stackPointer + 84, 0);
    Check(gears::GuestStackArgument32(memory.data(), stackPointer, 8) == 0,
        "a genuine zero reads as zero, so 'no overlapped' stays distinguishable"
        " from 'read the wrong place'");
}

} // namespace

int main()
{
    TestTheNinthArgumentIsAtEightyFour();
    TestReadsTheValueAtTheSlot();
    TestNeighbouringSlotsAreNotConfused();
    TestZeroIsAValueNotAFailure();

    if (g_failures == 0)
    {
        printf("all guest stack argument tests passed\n");
        return 0;
    }
    printf("%d guest stack argument test(s) FAILED\n", g_failures);
    return 1;
}
