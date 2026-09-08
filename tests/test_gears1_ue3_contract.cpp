#include <cstdio>
#include <cstdlib>

#include <x360ue3/engine_contract.hpp>

namespace
{

constexpr std::uint32_t kObjectId = 7U;
constexpr std::uint32_t kResourceId = 11U;
constexpr x360port::GuestAddress kBindingAddress = 0x82200000U;

void Require(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::abort();
    }
}

} // namespace

int main()
{
    const x360ue3::BindingSchema schema{.abi = {},
                                        .object_size = 16U,
                                        .fields = {{.name = "owner",
                                                    .guest_address = kBindingAddress,
                                                    .object_offset = 0U,
                                                    .width = 4U,
                                                    .kind = x360ue3::BindingKind::Pointer}}};
    Require(static_cast<bool>(x360ue3::ValidateBindingSchema(schema)),
            "Gears title binding schema was refused");

    x360ue3::FrameContract frame;
    Require(static_cast<bool>(frame.BeginFrame(1U)), "Gears UE3 frame did not begin");
    Require(static_cast<bool>(frame.RegisterObject(kObjectId)),
            "Gears UE3 object lifetime did not register");
    Require(static_cast<bool>(frame.RegisterResource(kResourceId)),
            "Gears UE3 resource lifetime did not register");
    Require(static_cast<bool>(frame.Append({.kind = x360ue3::RhiOperationKind::DrawIndexed,
                                            .sequence = 1U,
                                            .resource_id = kResourceId})),
            "Gears UE3 draw contract was refused");
    Require(static_cast<bool>(frame.Append(
                {.kind = x360ue3::RhiOperationKind::Present, .sequence = 2U, .resource_id = 0U})),
            "Gears UE3 present contract was refused");
    Require(static_cast<bool>(frame.EndFrame()), "Gears UE3 frame did not end");

    x360ue3::FrameContract negative;
    Require(static_cast<bool>(negative.BeginFrame(2U)), "negative Gears frame did not begin");
    const auto refused = negative.Append(
        {.kind = x360ue3::RhiOperationKind::Resolve, .sequence = 1U, .resource_id = kResourceId});
    Require(!static_cast<bool>(refused) && refused.error == x360ue3::ContractError::UnknownResource,
            "Gears UE3 unknown-resource path was accepted");
    std::puts("Gears UE3 contract: title binding and frame lifetime passed");
}
