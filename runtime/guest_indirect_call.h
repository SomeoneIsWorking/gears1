// A checked indirect call.
//
// The recompiler's default is a raw table index with no validation, so a guest
// address that is not a function entry reads a host function pointer out of
// whatever lies at that table offset and calls it. The #45 fault is exactly
// that: SIGSEGV at a host address two gigabytes below the guest mapping, from
// inside recompiled code, with nothing to say which guest pointer was bad.
//
// The console gave this for free -- a branch to a bad address takes an exception
// at a known guest address, and the bad address is right there in the report.
// This restores it, and it is the difference between "the process died in host
// memory" and "the title called through 0x41dec690, which is not code".
//
// Two things matter about the shape:
//
//   * IT MUST BE CHEAP. This is the hottest path in the recompiled image -- two
//     comparisons and an alignment test, inline, with the failure branch marked
//     unlikely and pushed out of line.
//   * IT MUST NOT CONTINUE. A table entry that happens to be non-null sends
//     execution into an unrelated guest function which then corrupts state
//     quietly, and the eventual crash is nowhere near the cause. Reporting and
//     stopping is what makes the bad call attributable.
#pragma once

#include "ppc_config.h"

// Claim the hook BEFORE ppc_context.h is parsed. That header leaves
// PPC_CALL_INDIRECT_FUNC #ifndef-guarded precisely so a runtime can take it
// over, and a macro body is not evaluated where it is defined -- so naming
// gears::CallGuestIndirect here, above its own declaration, is fine and keeps
// the whole mechanism in one file instead of split between here and CMake.
#define PPC_CALL_INDIRECT_FUNC(x) \
    gears::CallGuestIndirect(ctx, base, uint32_t(x))

#include "ppc_context.h"

namespace gears
{

// Is this guest address a plausible function entry? Inside the code section and
// four-byte aligned, since every PowerPC instruction is a word and an unaligned
// index lands between two table entries -- reading half of one host pointer and
// half of the next, which is how a bad target becomes an arbitrary jump.
inline bool IsValidGuestCallTarget(uint32_t target)
{
    if (target < PPC_CODE_BASE || target >= PPC_CODE_BASE + PPC_CODE_SIZE)
        return false;
    return (target & 3u) == 0;
}

// Reports the bad call with its guest address and the guest stack that made it,
// then stops the process. Out of line and cold: nothing on the fast path.
[[noreturn]] void ReportBadIndirectCall(uint32_t target, PPCContext& ctx,
                                        uint8_t* base);

// Replaces PPC_CALL_INDIRECT_FUNC. Defined for the recompiled sources through a
// compile definition and a forced include, so the generated code does not have
// to be touched -- it is regenerated from the image and any edit to it would be
// lost.
inline void CallGuestIndirect(PPCContext& ctx, uint8_t* base, uint32_t target)
{

    if (!IsValidGuestCallTarget(target)) [[unlikely]]
        ReportBadIndirectCall(target, ctx, base);

    PPCFunc* const function = PPC_LOOKUP_FUNC(base, target);

    // A null entry means the address is in range and four-byte aligned but is
    // not a function the recompiler found -- the middle of a function, or data
    // that happens to sit in the code section. Calling it would jump to zero.
    if (function == nullptr) [[unlikely]]
        ReportBadIndirectCall(target, ctx, base);

    function(ctx, base);
}

} // namespace gears
