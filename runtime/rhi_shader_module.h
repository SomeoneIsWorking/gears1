#pragma once

#include <cstdint>

namespace gears
{

// A title-neutral identity for one concrete Xenos microcode payload emitted by
// a retained title command-buffer or shader-state flush. The semantic comparer
// requires exactly one module per active stage; multiple entries preserve
// ambiguity instead of guessing.
struct RhiShaderModuleEvidence
{
    std::uint32_t guestAddress = 0;
    std::uint32_t sizeBytes = 0;
    std::uint64_t hash = 0;

    friend bool operator==(const RhiShaderModuleEvidence &,
                           const RhiShaderModuleEvidence &) = default;
};

} // namespace gears
