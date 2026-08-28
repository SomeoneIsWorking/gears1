#pragma once

#include "native_rhi.h"

#include <array>
#include <cstdint>
#include <map>

namespace gears::native_rhi
{

enum class ResourceRegistryStatus : std::uint8_t
{
    Accepted,
    AdoptionEvidenceMissing,
    ConstructionEvidenceMissing,
    InvalidObject,
    InvalidReferenceCount,
    DuplicateObject,
    UnknownObject,
    ResourceTypeMismatch,
    BackingObjectMismatch,
    ReferenceCountMismatch,
    IdentityMismatch,
    ReferenceBoundary,
};

struct ResourceRegistryResult
{
    ResourceRegistryStatus status = ResourceRegistryStatus::ConstructionEvidenceMissing;
    std::uint32_t referenceCount = 0;
};

// Title-neutral guest-object to native-resource identity and lifetime owner.
// It deliberately does not allocate Vulkan objects: a concrete backend uses
// the record as its stable identity while owning API-specific allocation and
// retirement. Unknown or boundary operations refuse instead of synthesizing
// resources that could hide a missing construction or destructor contract.
struct ResourceRecord
{
    RhiSemanticResourceConstructionKind kind = RhiSemanticResourceConstructionKind::OwnedBacking;
    std::uint32_t object = 0;
    std::uint32_t objectFlags = 0;
    std::uint32_t resourceType = 0;
    std::uint32_t backingObject = 0;
    std::uint32_t requestedBytes = 0;
    std::uint32_t resourceFlags = 0;
    std::uint32_t allocationFlags = 0;
    std::uint32_t referenceCount = 0;
    std::array<std::uint32_t, 5> objectWords{};
    bool constructionObserved = false;
};

class ResourceRegistry
{
  public:
    [[nodiscard]] ResourceRegistryResult AdoptExisting(const RhiResourceIdentityEvidence &identity);
    [[nodiscard]] ResourceRegistryResult Construct(const ResourceConstructionCommand &command);
    [[nodiscard]] ResourceRegistryResult ApplyLifetime(const ResourceLifetimeCommand &command);

    [[nodiscard]] const ResourceRecord *Find(std::uint32_t object) const;
    [[nodiscard]] std::size_t size() const noexcept { return resources_.size(); }
    void Reset() noexcept { resources_.clear(); }

  private:
    std::map<std::uint32_t, ResourceRecord> resources_;
};

[[nodiscard]] const char *ResourceRegistryStatusText(ResourceRegistryStatus status) noexcept;

} // namespace gears::native_rhi
