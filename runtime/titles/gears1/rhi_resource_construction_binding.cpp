#include "guest_state_memory.h"
#include "import_stub.h"
#include "rhi_semantic_stream.h"

#include <cstdint>

namespace
{

using RecompiledConstruction = void (*)(PPCContext &, std::uint8_t *);
using gears::titles::gears1::GuestStateMemory;

void ObserveResourceConstruction(PPCContext &ctx, std::uint8_t *base,
                                 gears::RhiSemanticResourceConstructionKind kind,
                                 RecompiledConstruction retained)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        retained(ctx, base);
        return;
    }

    const gears::RhiSemanticResourceConstruction construction{
        .kind = kind,
        .requestedBytes = ctx.r3.u32,
        .resourceFlags = ctx.r4.u32,
        .allocationFlags = ctx.r5.u32,
    };
    retained(ctx, base);

    gears::RhiResourceConstructionEvidence evidence{.object = ctx.r3.u32};
    if (evidence.object != 0)
    {
        const GuestStateMemory memory(base);
        evidence.present = true;
        evidence.objectWords = {
            memory.Read32(evidence.object + 0),  memory.Read32(evidence.object + 4),
            memory.Read32(evidence.object + 20), memory.Read32(evidence.object + 24),
            memory.Read32(evidence.object + 28),
        };
    }
    gears::ObserveRhiSemanticResourceConstruction(construction, evidence);
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_8222EA18);
PPC_FUNC(sub_8222EA18)
{
    ObserveResourceConstruction(ctx, base, gears::RhiSemanticResourceConstructionKind::OwnedBacking,
                                __imp__sub_8222EA18);
}

extern "C" PPC_FUNC(__imp__sub_8222EB78);
PPC_FUNC(sub_8222EB78)
{
    ObserveResourceConstruction(
        ctx, base, gears::RhiSemanticResourceConstructionKind::WrappedBacking, __imp__sub_8222EB78);
}
