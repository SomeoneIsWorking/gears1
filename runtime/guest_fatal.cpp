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

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_memory.h"

// TWO ENTRY POINTS, EIGHT BYTES APART, into the same body -- and the caller
// uses the SECOND. Overriding only sub_828D2FB0 caught nothing while the panic
// fired anyway, which is what exposed this: both sites that reach KeBugCheck(0)
// share the return address 0x828D30B0, but one lives in the range the
// recompiler labelled sub_828D2FB0 and the other in sub_828D2FB8. Whatever
// registers this handler registers the +8 entry.
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
void ReportEntry(const char* entry, const PPCContext& ctx)
{
    const uint32_t flag = ByteSwap(*gears::Memory().Translate<uint32_t>(kFatalReentryFlag));
    lucent::error("fatal", "the title entered its fatal handler ({}) from {:#x}:"
        " r3={:#x} r4={:#x} r5={:#x} r6={:#x}, re-entry flag {} -- this entry {}",
        entry, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        flag,
        flag == 1 ? "WILL bug-check" : "is the first, and something already went"
                                       " wrong to get here");
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
