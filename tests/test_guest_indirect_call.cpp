// Tests for the guest indirect-call target check.
//
// WHAT THIS PREVENTS. The recompiler's indirect call is a raw table index with
// no validation:
//
//   #define PPC_LOOKUP_FUNC(base, y) \
//       *(PPCFunc**)(base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + \
//                    (uint64_t(uint32_t(y) - PPC_CODE_BASE) * 2))
//
// so a guest address that is not a function entry reads a host function pointer
// out of whatever happens to lie at that offset and CALLS it. The captured fault
// from #45 is exactly that: SIGSEGV at 0x7fbeb1c001fe, two gigabytes below the
// guest mapping, from inside a recompiled function -- a jump into host memory.
//
// On the console the same mistake takes an exception at a known guest address
// and is immediately attributable. Here it becomes an unattributable host crash,
// and worse, a table entry that happens to be non-null sends execution into an
// unrelated guest function that then corrupts state quietly. The check restores
// what the hardware gave for free: the bad address, named, at the moment of use.
//
// The bounds are the generated ones, so the test pins the real geometry rather
// than a copy of it -- a regenerated image with a different code range must not
// silently leave this test asserting yesterday's addresses.

#include <cstdio>
#include <cstdint>

#include "ppc_config.h"
#include "guest_indirect_call.h"

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

void TestRealCodeRangeIsAccepted()
{
    Check(gears::IsValidGuestCallTarget(uint32_t(PPC_CODE_BASE)),
        "range: the first byte of the code section is a valid target");
    Check(gears::IsValidGuestCallTarget(
              uint32_t(PPC_CODE_BASE + PPC_CODE_SIZE - 4)),
        "range: the last instruction slot is a valid target");
    Check(gears::IsValidGuestCallTarget(0x824961D0u),
        "range: a function the title actually calls is accepted -- if this fails"
        " the check would reject real work and break the title");
}

void TestOutsideTheCodeRangeIsRejected()
{
    Check(!gears::IsValidGuestCallTarget(uint32_t(PPC_CODE_BASE - 4)),
        "range: one word below the code section is rejected");
    Check(!gears::IsValidGuestCallTarget(
              uint32_t(PPC_CODE_BASE + PPC_CODE_SIZE)),
        "range: one past the end is rejected");
    Check(!gears::IsValidGuestCallTarget(0u),
        "range: a null target is rejected -- and this is the common case, since"
        " a freed object's vtable slot reads as zero once the memory is reused");
    Check(!gears::IsValidGuestCallTarget(0xFFFFFFFFu),
        "range: an all-ones target is rejected");
}

// The values seen in the real crash. A garbage pointer read out of recycled
// memory looks like heap data, not like a code address, and each of these would
// previously have been indexed into the table and called.
void TestGarbageFromRecycledMemoryIsRejected()
{
    const uint32_t garbage[] = {
        0x00470075u,    // the misread holder value from an earlier probe
        0x42babc40u,    // a pool block address
        0x41dec690u,    // the cached object from the crash
        0x0000000cu,    // a small integer where a pointer was expected
        0x80000000u,    // the kernel half of the address space
    };
    for (const uint32_t value : garbage)
    {
        if (gears::IsValidGuestCallTarget(value))
        {
            printf("FAIL garbage: %#x was accepted as a call target\n", value);
            ++g_failures;
        }
    }
}

// Alignment. Every PowerPC instruction is four bytes, so a call to an unaligned
// address cannot be a function entry -- and an unaligned index into a table of
// pairs lands between entries, reading half of one pointer and half of another.
void TestUnalignedTargetsAreRejected()
{
    Check(!gears::IsValidGuestCallTarget(uint32_t(PPC_CODE_BASE) + 1),
        "alignment: an unaligned address inside the code range is still invalid,"
        " because it cannot be an instruction boundary");
    Check(!gears::IsValidGuestCallTarget(uint32_t(PPC_CODE_BASE) + 2),
        "alignment: two bytes in is invalid too");
    Check(gears::IsValidGuestCallTarget(uint32_t(PPC_CODE_BASE) + 4),
        "alignment: and the next aligned word is fine");
}

} // namespace

int main()
{
    TestRealCodeRangeIsAccepted();
    TestOutsideTheCodeRangeIsRejected();
    TestGarbageFromRecycledMemoryIsRejected();
    TestUnalignedTargetsAreRejected();

    printf("code range checked: %#llx .. %#llx\n",
        (unsigned long long)PPC_CODE_BASE,
        (unsigned long long)(PPC_CODE_BASE + PPC_CODE_SIZE));

    if (g_failures == 0)
    {
        printf("all guest indirect call tests passed\n");
        return 0;
    }
    printf("%d guest indirect call test(s) FAILED\n", g_failures);
    return 1;
}
