#include "guest_indirect_call.h"

#include <cstdio>
#include <cstdlib>

#include <lucent/log.h>

#include "guest_backtrace.h"
#include "guest_memory.h"
#include "guest_thread.h"

namespace gears
{

[[noreturn]] void
ReportGuestIndirectCallFailure(uint32_t target, PPCContext &ctx, uint8_t *base,
                               GuestIndirectCallContextReporter reportTitleContext)
{

    // WHY THIS IS AN ERROR AND NOT A WARNING-AND-CONTINUE. There is no way to
    // continue honestly: the title asked to call an address that is not code,
    // so any choice made here -- skip the call, return zero, call something
    // else -- invents behaviour the original never had, and does it silently at
    // the exact moment the state is already wrong. The value of this report is
    // that it names the bad address while the stack that produced it still
    // exists.
    lucent::error("call",
                  "INDIRECT CALL TO {:#x}, WHICH IS NOT A GUEST FUNCTION"
                  " (code section is {:#x}..{:#x}). The title branched through a pointer"
                  " that is not code -- typically a vtable slot read out of memory that has"
                  " already been recycled",
                  target, uint32_t(PPC_CODE_BASE), uint32_t(PPC_CODE_BASE + PPC_CODE_SIZE));

    lucent::error("call", "  on guest thread '{}', lr {:#x}, r1 {:#x}", GuestThreadName(), ctx.lr,
                  ctx.r1.u32);

    // THE CALLER IS THE ANSWER, not the callee. The bad target says what went
    // wrong; the guest stack says who did it, which is the part that names the
    // code to fix. Reported as a walker failure when the walk is too short to be
    // an answer, rather than printing three frames as though they were the whole
    // story.
    const std::string trace = FormatGuestBacktrace(ctx.r1.u32, uint32_t(ctx.lr));
    lucent::error("call", "  guest stack: {}", trace);

    // THE REGISTERS AND WHAT THEY POINT AT. A bad target on its own cannot
    // distinguish the three things that produce one: an object pointer that is
    // garbage, a valid object whose vtable pointer is garbage, or a valid vtable
    // whose slot was never populated. The words around the pointers separate
    // them, and at the moment of the abort they are still intact.
    {
        lucent::Line registers;
        registers.add("  guest registers:");
        const uint32_t values[] = {
            ctx.r3.u32,  ctx.r4.u32,  ctx.r5.u32,  ctx.r10.u32,
            ctx.r11.u32, ctx.r24.u32, ctx.r30.u32, ctx.r31.u32,
        };
        const char *names[] = {"r3", "r4", "r5", "r10", "r11", "r24", "r30", "r31"};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
            registers.add(" {}={:#x}", names[i], values[i]);
        registers.flush(lucent::Level::Error, "call");
    }

    // WHAT COUNTS AS DUMPABLE. The first version of this bounded the dump to the
    // loaded IMAGE and then said, of an address outside it, "that alone makes it
    // not an object" -- which is false and was printed with confidence. Heap
    // objects live in the physical heap well outside the image, and the object
    // in a real crash may be one. The bound that actually matters is the guest
    // mapping: inside it the read cannot fault, and outside it the address
    // cannot be a guest pointer at all.
    const auto readable = [](uint32_t address) { return uint64_t(address) + 64 < PPC_MEMORY_SIZE; };
    const auto dump = [&](const char *what, uint32_t address)
    {
        if (!readable(address))
        {
            lucent::error("call",
                          "  {} {:#x} is outside guest memory entirely,"
                          " so it is not a guest pointer",
                          what, address);
            return;
        }
        lucent::Line row;
        row.add("  {} {:#x}:", what, address);
        for (uint32_t offset = 0; offset < 64; offset += 4)
        {
            const uint32_t word =
                __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + address + offset));
            row.add(" {:08x}", word);
        }
        row.flush(lucent::Level::Error, "call");
    };

    DispatchGuestIndirectCallContext(reportTitleContext, target, ctx, base);

    dump("object r3", ctx.r3.u32);
    if (readable(ctx.r3.u32))
    {
        const uint32_t vtable =
            __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ctx.r3.u32));
        lucent::error("call", "  its first word (the vtable pointer) is {:#x}", vtable);
        dump("vtable", vtable);
    }

    // Flush before dying: the report above is the entire point, and a buffered
    // sink would drop it.
    std::fflush(nullptr);

    // abort() rather than a return, so the process stops at the cause with a
    // core dump. The fault reporter handles SIGABRT too, so the host backtrace
    // lands next to this.
    std::abort();
}

} // namespace gears
