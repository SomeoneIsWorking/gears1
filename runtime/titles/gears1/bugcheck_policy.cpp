#include "bugcheck_policy.h"

#include <array>

namespace gears
{
namespace
{

constexpr std::array kClassifications{
    GuestBugCheckClassification{
        .code = 0,
        .returnAddress = 0x828D30B0,
        .description = "the tail of the terminate path _purecall runs into, so"
                       " look above for the PURE VIRTUAL CALL that got here",
    },
};

constexpr GuestBugCheckPolicy kPolicy{.classifications = kClassifications};
static_assert(IsValidGuestBugCheckPolicy(kPolicy));

} // namespace

const GuestBugCheckPolicy &LinkedGuestBugCheckPolicy()
{
    return kPolicy;
}

} // namespace gears
