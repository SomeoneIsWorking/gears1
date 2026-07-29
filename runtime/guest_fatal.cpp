// A trace on the title's own fatal-error path.
//
// The title bug-checks intermittently about 70 seconds in (catalog #44), and
// the panic itself is a RE-ENTRANCY GUARD: sub_828D2FB0 reads a flag, calls
// KeBugCheck(0) if it is already set, and otherwise sets it. So the panic is
// the handler being entered a SECOND time, and the interesting event -- the
// first entry, and whatever caused it -- happens silently before anything the
// runtime logs.
//
// The seam is XenonRecomp's: every guest function is emitted as a weak alias
// `sub_X` over the real body `__imp__sub_X`, so a strong `sub_X` here replaces
// it and can call through. PPCFuncMappings, which serves INDIRECT calls, holds
// the alias too -- so this catches the handler even though nothing in the image
// calls it directly, which is how it is reached.
#include "import_stub.h"

#include <string>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_memory.h"

// TWO ENTRY POINTS, EIGHT BYTES APART, into the same body -- and the caller
// uses the SECOND. Overriding only sub_828D2FB0 caught nothing while the panic
// fired anyway, which is what exposed this: both sites that reach KeBugCheck(0)
// share the return address 0x828D30B0, but one lives in the range the
// recompiler labelled sub_828D2FB0 and the other in sub_828D2FB8. Whatever
// registers this handler registers the +8 entry.
// The chain, from the outside in. Everything below sub_828D0790 is
// unconditional -- sub_828D9FD8 always ends in the fatal exit -- so the DECISION
// is made above sub_828D0790, and that is the only one of these whose caller is
// still unknown. It has no direct callers anywhere in the image, so like the
// handler it is reached through a pointer, and like the handler it can be
// caught here.
extern "C" PPC_FUNC(__imp__sub_828D0790);
extern "C" PPC_FUNC(__imp__sub_828D2FB0);
extern "C" PPC_FUNC(__imp__sub_828D2FB8);

namespace
{
// The re-entrancy flag the handler tests. Reading it on the way IN says whether
// this entry is the one that panics, which is the difference between watching
// the failure and watching its aftermath.
constexpr uint32_t kFatalReentryFlag = 0x82BC9E18;
} // namespace

namespace
{
// Printable runs in a window of guest memory, reported as candidates rather
// than as fact: this is a stack frame, so some of what looks like text will be
// coincidence. Naming it a guess is the difference between a lead and a lie.
void DumpNearbyText(uint32_t around)
{
    if (around == 0)
        return;
    constexpr int32_t kBefore = 512;
    constexpr int32_t kAfter = 2048;
    const uint8_t* memory = gears::Memory().Base();

    std::string run;
    uint32_t runStart = 0;
    uint32_t found = 0;
    for (int32_t offset = -kBefore; offset <= kAfter && found < 8; ++offset)
    {
        const uint32_t address = uint32_t(int64_t(around) + offset);
        const unsigned char c = memory[address];
        if (c >= 0x20 && c <= 0x7E)
        {
            if (run.empty())
                runStart = address;
            run.push_back(char(c));
            continue;
        }
        if (run.size() >= 8)
        {
            lucent::error("fatal", "  text near the fatal frame at {:#x}: \"{}\"",
                          runStart, run);
            ++found;
        }
        run.clear();
    }
}

void ReportEntry(const char* entry, const PPCContext& ctx)
{
    const uint32_t flag = ByteSwap(*gears::Memory().Translate<uint32_t>(kFatalReentryFlag));
    lucent::error("fatal", "the title entered its fatal handler ({}) from {:#x}:"
        " r3={:#x} r4={:#x} r5={:#x} r6={:#x}, re-entry flag {} -- this entry {}",
        entry, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        flag,
        flag == 1 ? "WILL bug-check" : "is the first, and something already went"
                                       " wrong to get here");

    // The caller (sub_828D9FD8) builds something in its stack frame before
    // handing control here, and the calls it makes on the way look like
    // formatting. If that is an error message it names the failure outright,
    // which is worth more than every register in the report above. Scanning is
    // cheap and happens exactly once per process death.
    DumpNearbyText(ctx.r6.u32);
}
} // namespace

PPC_FUNC(sub_828D2FB0)
{
    ReportEntry("sub_828D2FB0", ctx);
    __imp__sub_828D2FB0(ctx, base);
}

PPC_FUNC(sub_828D2FB8)
{
    ReportEntry("sub_828D2FB8", ctx);
    __imp__sub_828D2FB8(ctx, base);
}

// sub_828D3118 is `r5 = 0; r4 = 1; goto handler` and sub_828D3128 is
// `r5 = 1; ...` -- and the handler panics precisely when that argument is zero.
// So the KeBugCheck is HARDCODED into this path by the title: it is the "must
// not return" tail of a deliberate terminate, not a field the runtime failed to
// populate. Two wrappers, one that ends the process and one that does not.
PPC_FUNC(sub_828D0790)
{
    // The top of the shutdown chain: runs a registered handler, then reports,
    // then dies. Its caller is what actually decided to end the process, and
    // the link register is the only thing that names it.
    lucent::error("fatal", "the title began shutting down (sub_828D0790) from"
        " {:#x}: r3={:#x} r4={:#x} r5={:#x} r6={:#x}", uint32_t(ctx.lr),
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    __imp__sub_828D0790(ctx, base);
}
