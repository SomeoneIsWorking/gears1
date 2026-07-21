#pragma once

#include "implemented_imports.h"
#include "ppc_config.h"
#include "ppc_context.h"

namespace gears
{
// Reports an import the runtime does not implement yet and aborts. Returning a
// plausible-looking value instead would let the game continue into undefined
// behaviour, and the resulting crash would be nowhere near the real cause.
[[noreturn]] void UnimplementedImport(const char* name, const PPCContext& ctx);
} // namespace gears

// The generated code declares imports with PPC_EXTERN_FUNC, i.e. ordinary C++
// linkage, so the definitions must be mangled to match.
#define GEARS_UNIMPLEMENTED_IMPORT(name)                        \
    void __imp__##name(PPCContext& __restrict ctx, uint8_t*)    \
    {                                                           \
        gears::UnimplementedImport(#name, ctx);                 \
    }
