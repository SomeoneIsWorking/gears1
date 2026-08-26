#include "rhi_semantic_stream.h"

#include <cassert>
#include <cstdlib>

#include <lucent/config.h>

int main()
{
    using gears::CompareRhiDrawPacket;
    using gears::RhiDrawEvidenceResult;
    using gears::RhiDrawPacketEvidence;
    using gears::RhiSemanticDraw;
    using gears::RhiSemanticDrawKind;

    const RhiSemanticDraw autoIndexed{
        .kind = RhiSemanticDrawKind::BoundVertices,
        .primitiveType = 4,
        .elementCount = 300,
    };
    const RhiDrawPacketEvidence matchingAuto{
        .present = true,
        .opcode = 0x22,
        .primitiveType = 4,
        .sourceSelect = 2,
        .elementCount = 300,
    };
    assert(CompareRhiDrawPacket(autoIndexed, matchingAuto) == RhiDrawEvidenceResult::Match);

    RhiDrawPacketEvidence altered = matchingAuto;
    altered.elementCount = 299;
    assert(CompareRhiDrawPacket(autoIndexed, altered) == RhiDrawEvidenceResult::Mismatch);
    altered = matchingAuto;
    altered.present = false;
    assert(CompareRhiDrawPacket(autoIndexed, altered) == RhiDrawEvidenceResult::Missing);

    const RhiSemanticDraw indexed{
        .kind = RhiSemanticDrawKind::BoundIndices,
        .primitiveType = 3,
        .elementCount = 42,
    };
    const RhiDrawPacketEvidence matchingIndexed{
        .present = true,
        .opcode = 0x22,
        .primitiveType = 3,
        .sourceSelect = 0,
        .elementCount = 42,
    };
    assert(CompareRhiDrawPacket(indexed, matchingIndexed) == RhiDrawEvidenceResult::Match);
    altered = matchingIndexed;
    altered.sourceSelect = 2;
    assert(CompareRhiDrawPacket(indexed, altered) == RhiDrawEvidenceResult::Mismatch);

    assert(setenv("GEARS_NATIVE_RHI_OBSERVE", "1", 1) == 0);
    lucent::config::set_prefix("GEARS_");
    gears::ObserveRhiSemanticDraw(autoIndexed, matchingAuto);
    gears::ObserveRhiSemanticDraw(indexed, altered);
    const gears::RhiSemanticFrame frame = gears::SealRhiSemanticFrame(17);
    assert(frame.frameSequence == 17);
    assert(frame.draws.size() == 2);
    assert(frame.draws[0].sequence + 1 == frame.draws[1].sequence);
    assert(frame.matched == 1);
    assert(frame.missing == 0);
    assert(frame.mismatched == 1);

    return 0;
}
