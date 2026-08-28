#pragma once

#include <cstdint>
#include <optional>

namespace gears
{

enum class RhiResourceLifetimeOperation : std::uint8_t
{
    AddReference,
    Release,
};

// Applies only the non-boundary reference-count transition. A zero-to-one add
// and a one-to-zero release have backing-object/destructor semantics owned by
// the retained title body and deliberately return no result here.
[[nodiscard]] std::optional<std::uint32_t>
TryApplyNativeRhiReferenceFastPath(std::uint8_t *guestBase, std::uint32_t object,
                                   RhiResourceLifetimeOperation operation);

} // namespace gears
