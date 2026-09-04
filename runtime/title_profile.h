#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace gears
{

// Both digests are produced by local provisioning. The container digest names
// the exact input XEX, while the image digest names the executable bytes that
// the runtime image and exact-revision bindings describe.
using Sha256Digest = std::array<std::uint8_t, 32>;

struct XexIdentity
{
    Sha256Digest containerDigest{};
    Sha256Digest imageDigest{};
    std::uint32_t imageBase = 0;
    std::uint32_t imageSize = 0;
    std::uint32_t entryPoint = 0;

    [[nodiscard]] bool operator==(const XexIdentity &) const = default;
};

enum class RevisionStatus : std::uint8_t
{
    Recognized,
    Experimental,
    Verified,
};

enum class TitleCapability : std::uint8_t
{
    DynarecExecution,
    HeadlessBoot,
    ContentMount,
    OracleComparison,
    Gameplay,
    CompatibilityRenderer,
    NativeRhi,
    Count,
};

enum class CapabilityStatus : std::uint8_t
{
    Unavailable,
    Untested,
    Verified,
};

struct TitleCapabilities
{
    std::array<CapabilityStatus, static_cast<std::size_t>(TitleCapability::Count)> status{};

    [[nodiscard]] CapabilityStatus Get(TitleCapability capability) const noexcept;
};

// Exact-revision adapters supply instances after x360port has parsed the
// user-owned runtime image. The shared runtime owns this schema and its
// fail-closed selection rules.
struct TitleProfile
{
    std::string_view titleKey;
    std::string_view revisionKey;
    XexIdentity xex;
    RevisionStatus revisionStatus = RevisionStatus::Recognized;
    TitleCapabilities capabilities;
    std::string_view saveNamespace;
};

enum class TitleProfileError : std::uint8_t
{
    None,
    InvalidObservedIdentity,
    InvalidProfile,
    AmbiguousRegistry,
    UnknownBuild,
};

struct TitleProfileResolution
{
    const TitleProfile *profile = nullptr;
    TitleProfileError error = TitleProfileError::UnknownBuild;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return profile != nullptr && error == TitleProfileError::None;
    }
};

// The returned pointer refers to an element of profiles and remains valid only
// as long as that profile storage does. Every registry entry is validated
// before a match is returned, so malformed or duplicate revision bindings
// cannot activate a partially valid profile.
[[nodiscard]] TitleProfileResolution ResolveTitleProfile(std::span<const TitleProfile> profiles,
                                                         const XexIdentity &observed) noexcept;

[[nodiscard]] std::string_view TitleProfileErrorText(TitleProfileError error) noexcept;

} // namespace gears
