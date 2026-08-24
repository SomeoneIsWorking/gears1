#pragma once

#include <span>

#include "title_profile.h"

namespace gears
{

// Exact profiles compiled alongside the locally generated PPC module. The
// generator supplies the executable identity in ppc_config.h; tracked runtime
// source supplies only title policy and capability claims.
[[nodiscard]] std::span<const TitleProfile> GeneratedTitleProfiles() noexcept;

} // namespace gears
