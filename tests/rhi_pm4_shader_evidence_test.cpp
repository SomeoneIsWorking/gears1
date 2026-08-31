#include "rhi_pm4_shader_evidence_test.h"

#include "rhi_pm4_shader_evidence.h"

#include <cassert>
#include <cstdint>
#include <optional>

void RunRhiPm4ShaderEvidenceTests()
{
    constexpr std::uint64_t vertexHash = 0x1122334455667788ull;
    constexpr std::uint64_t pixelHash = 0x123456789ABCDEF0ull;
    const gears::RhiSemanticBinding pixelShader{
        .kind = gears::RhiSemanticBindingKind::PixelShader,
        .object = 0x40107800,
        .shaderModules = {{.guestAddress = 0x10200, .sizeBytes = 72, .hash = pixelHash}},
    };
    const gears::RhiSemanticBinding vertexShader{
        .kind = gears::RhiSemanticBindingKind::VertexShader,
        .object = 0x40107000,
        .shaderModules = {{.guestAddress = 0x10000, .sizeBytes = 48, .hash = vertexHash}},
    };
    gears::RhiObservedDraw observed{
        .state = {.draw = {.kind = gears::RhiSemanticDrawKind::BoundVertices,
                           .primitiveType = 4,
                           .elementCount = 300}},
        .packet = {.present = true, .packetGuestAddress = 0xA0010000},
        .evidence = gears::RhiDrawEvidenceResult::Match,
    };
    observed.state.pixelShader = pixelShader;
    observed.state.vertexShader = vertexShader;
    const gears::RhiSemanticFrame semantic{
        .frameSequence = 5000,
        .events = {{.payload = observed}},
    };
    assert(!gears::ObserveRhiSemanticFrameForPm4ShaderEvidence(semantic).has_value());
    const std::optional<gears::RhiPm4ShaderFrameComparison> joined =
        gears::PublishRhiPm4FrameShaderEvidence(5000, {{.packetGuestAddress = 0x00010000,
                                                        .packetBufferBase = 0x00010000,
                                                        .vertexShaderHash = vertexHash,
                                                        .pixelShaderHash = pixelHash}});
    assert(joined.has_value());
    assert(joined->matched == 1);
    assert(joined->missing == 0);
    assert(joined->mismatched == 0);

    assert(!gears::PublishRhiPm4FrameShaderEvidence(5001, {{.packetGuestAddress = 0x00010000,
                                                            .vertexShaderHash = vertexHash,
                                                            .pixelShaderHash = pixelHash ^ 1}})
                .has_value());
    gears::RhiSemanticFrame alteredSemantic = semantic;
    alteredSemantic.frameSequence = 5001;
    const std::optional<gears::RhiPm4ShaderFrameComparison> mismatch =
        gears::ObserveRhiSemanticFrameForPm4ShaderEvidence(alteredSemantic);
    assert(mismatch.has_value());
    assert(mismatch->mismatched == 1);
    assert(mismatch->missing == 0);

    gears::RhiSemanticFrame missingSemantic = semantic;
    missingSemantic.frameSequence = 5002;
    assert(!gears::PublishRhiPm4FrameShaderEvidence(5002, {}).has_value());
    const std::optional<gears::RhiPm4ShaderFrameComparison> missing =
        gears::ObserveRhiSemanticFrameForPm4ShaderEvidence(missingSemantic);
    assert(missing.has_value());
    assert(missing->missing == 1);

    gears::RhiSemanticFrame expiredSemantic = semantic;
    expiredSemantic.frameSequence = 6000;
    assert(!gears::ObserveRhiSemanticFrameForPm4ShaderEvidence(expiredSemantic).has_value());
    assert(!gears::PublishRhiPm4FrameShaderEvidence(6065, {}).has_value());
    const std::optional<gears::RhiPm4ShaderFrameComparison> expiredDuplicate =
        gears::PublishRhiPm4FrameShaderEvidence(6000, {});
    assert(expiredDuplicate.has_value());
    assert(expiredDuplicate->duplicate);
}
