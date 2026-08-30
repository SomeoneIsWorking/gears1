#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace gears
{

struct GuestBugCheckClassification
{
    std::uint32_t code = 0;
    std::uint32_t returnAddress = 0;
    std::string_view description{};
};

struct GuestBugCheckPolicy
{
    std::span<const GuestBugCheckClassification> classifications{};
};

// An empty policy is valid for a title with no grounded site classifications.
// A classified site must identify a real return address, explain its meaning,
// and be unique by the complete (code, return-address) key.
[[nodiscard]] constexpr bool IsValidGuestBugCheckPolicy(const GuestBugCheckPolicy &policy) noexcept
{
    for (std::size_t index = 0; index < policy.classifications.size(); ++index)
    {
        const GuestBugCheckClassification &classification = policy.classifications[index];
        if (classification.returnAddress == 0 || classification.description.empty())
            return false;

        for (std::size_t previous = 0; previous < index; ++previous)
        {
            const GuestBugCheckClassification &candidate = policy.classifications[previous];
            if (candidate.code == classification.code &&
                candidate.returnAddress == classification.returnAddress)
                return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::string_view
FindGuestBugCheckDescription(const GuestBugCheckPolicy &policy, std::uint32_t code,
                             std::uint32_t returnAddress) noexcept
{
    if (!IsValidGuestBugCheckPolicy(policy))
        return {};

    for (const GuestBugCheckClassification &classification : policy.classifications)
    {
        if (classification.code == code && classification.returnAddress == returnAddress)
            return classification.description;
    }
    return {};
}

// One executable links one exact title adapter. Each adapter supplies this
// strong definition; accidentally linking two adapters is therefore a link
// error rather than an ambiguous runtime selection.
[[nodiscard]] const GuestBugCheckPolicy &LinkedGuestBugCheckPolicy();

// Shared fail-closed access to the linked exact-title classifications.
[[nodiscard]] std::string_view CurrentGuestBugCheckDescription(std::uint32_t code,
                                                               std::uint32_t returnAddress);

} // namespace gears
