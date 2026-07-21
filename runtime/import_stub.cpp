#include "import_stub.h"

#include <cstdlib>

#include <lucent/log.h>

namespace gears
{

void UnimplementedImport(const char* name, const PPCContext& ctx)
{
    lucent::error("import", "unimplemented: {}", name);
    // The first eight argument registers are usually enough to tell what the
    // game wanted, which is the useful part of hitting one of these.
    lucent::error("import", "  r3={:#x} r4={:#x} r5={:#x} r6={:#x}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    lucent::error("import", "  r7={:#x} r8={:#x} r9={:#x} r10={:#x}",
        ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
    abort();
}

} // namespace gears
