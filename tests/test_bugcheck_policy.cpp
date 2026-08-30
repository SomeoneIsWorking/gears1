#include "bugcheck_policy.h"

#include <array>
#include <cassert>

namespace
{
constexpr gears::GuestBugCheckPolicy kEmpty{};
static_assert(gears::IsValidGuestBugCheckPolicy(kEmpty));
static_assert(gears::FindGuestBugCheckDescription(kEmpty, 0, 1).empty());

constexpr std::array kValidClassifications{
    gears::GuestBugCheckClassification{.code = 0, .returnAddress = 1, .description = "zero"},
    gears::GuestBugCheckClassification{.code = 1, .returnAddress = 1, .description = "one"},
    gears::GuestBugCheckClassification{.code = 0, .returnAddress = 2, .description = "other"},
};
constexpr gears::GuestBugCheckPolicy kValid{.classifications = kValidClassifications};
static_assert(gears::IsValidGuestBugCheckPolicy(kValid));
static_assert(gears::FindGuestBugCheckDescription(kValid, 0, 1) == "zero");
static_assert(gears::FindGuestBugCheckDescription(kValid, 1, 1) == "one");
static_assert(gears::FindGuestBugCheckDescription(kValid, 0, 2) == "other");
static_assert(gears::FindGuestBugCheckDescription(kValid, 2, 1).empty());

constexpr std::array kMissingAddress{
    gears::GuestBugCheckClassification{.code = 0, .description = "missing address"},
};
static_assert(!gears::IsValidGuestBugCheckPolicy({.classifications = kMissingAddress}));

constexpr std::array kMissingDescription{
    gears::GuestBugCheckClassification{.code = 0, .returnAddress = 1},
};
static_assert(!gears::IsValidGuestBugCheckPolicy({.classifications = kMissingDescription}));

constexpr std::array kDuplicate{
    gears::GuestBugCheckClassification{.code = 0, .returnAddress = 1, .description = "first"},
    gears::GuestBugCheckClassification{.code = 0, .returnAddress = 1, .description = "second"},
};
constexpr gears::GuestBugCheckPolicy kDuplicatePolicy{.classifications = kDuplicate};
static_assert(!gears::IsValidGuestBugCheckPolicy(kDuplicatePolicy));
static_assert(gears::FindGuestBugCheckDescription(kDuplicatePolicy, 0, 1).empty());
} // namespace

int main()
{
    const gears::GuestBugCheckPolicy &gears1 = gears::LinkedGuestBugCheckPolicy();
    assert(gears::IsValidGuestBugCheckPolicy(gears1));
    assert(gears::FindGuestBugCheckDescription(gears1, 0, 0x828D30B0) ==
           "the tail of the terminate path _purecall runs into, so"
           " look above for the PURE VIRTUAL CALL that got here");
    assert(gears::FindGuestBugCheckDescription(gears1, 1, 0x828D30B0).empty());
    assert(gears::FindGuestBugCheckDescription(gears1, 0, 0x828D30B4).empty());
    assert(gears::CurrentGuestBugCheckDescription(0, 0x828D30B0) ==
           "the tail of the terminate path _purecall runs into, so"
           " look above for the PURE VIRTUAL CALL that got here");
    assert(gears::CurrentGuestBugCheckDescription(1, 0x828D30B0).empty());
}
