#include "native_rhi_resources.h"

namespace gears::native_rhi
{
namespace
{

constexpr std::uint32_t kResourceTypeMask = 0xF;

ResourceRegistryResult Result(ResourceRegistryStatus status, std::uint32_t referenceCount = 0)
{
    return {.status = status, .referenceCount = referenceCount};
}

} // namespace

ResourceRegistryResult ResourceRegistry::Construct(const ResourceConstructionCommand &command)
{
    const RhiResourceConstructionEvidence &evidence = command.retained;
    if (!evidence.present)
        return Result(ResourceRegistryStatus::ConstructionEvidenceMissing);
    if (evidence.object == 0)
        return Result(ResourceRegistryStatus::InvalidObject);
    if (evidence.initialReferenceCount == 0)
        return Result(ResourceRegistryStatus::InvalidReferenceCount);
    if (resources_.contains(evidence.object))
        return Result(ResourceRegistryStatus::DuplicateObject);

    resources_.emplace(evidence.object,
                       ResourceRecord{.kind = command.construction.kind,
                                      .object = evidence.object,
                                      .objectFlags = evidence.objectFlags,
                                      .resourceType = evidence.objectFlags & kResourceTypeMask,
                                      .backingObject = evidence.backingObject,
                                      .requestedBytes = command.construction.requestedBytes,
                                      .resourceFlags = command.construction.resourceFlags,
                                      .allocationFlags = command.construction.allocationFlags,
                                      .referenceCount = evidence.initialReferenceCount,
                                      .objectWords = evidence.objectWords});
    return Result(ResourceRegistryStatus::Accepted, evidence.initialReferenceCount);
}

ResourceRegistryResult ResourceRegistry::ApplyLifetime(const ResourceLifetimeCommand &command)
{
    const RhiSemanticResourceLifetime &lifetime = command.lifetime;
    const auto resource = resources_.find(lifetime.object);
    if (resource == resources_.end())
        return Result(ResourceRegistryStatus::UnknownObject);

    ResourceRecord &record = resource->second;
    if (lifetime.resourceType != (record.objectFlags & kResourceTypeMask))
        return Result(ResourceRegistryStatus::ResourceTypeMismatch, record.referenceCount);
    if (lifetime.backingObject != record.backingObject)
        return Result(ResourceRegistryStatus::BackingObjectMismatch, record.referenceCount);
    if (lifetime.previousReferenceCount != record.referenceCount)
        return Result(ResourceRegistryStatus::ReferenceCountMismatch, record.referenceCount);

    if (lifetime.operation == RhiResourceLifetimeOperation::AddReference)
    {
        if (record.referenceCount == UINT32_MAX)
            return Result(ResourceRegistryStatus::ReferenceBoundary, record.referenceCount);
        ++record.referenceCount;
        return Result(ResourceRegistryStatus::Accepted, record.referenceCount);
    }

    // The title's retained release body owns the one-to-zero destructor and
    // backing-resource transition. A native backend cannot skip that boundary.
    if (record.referenceCount <= 1)
        return Result(ResourceRegistryStatus::ReferenceBoundary, record.referenceCount);
    --record.referenceCount;
    return Result(ResourceRegistryStatus::Accepted, record.referenceCount);
}

const ResourceRecord *ResourceRegistry::Find(std::uint32_t object) const
{
    const auto resource = resources_.find(object);
    return resource == resources_.end() ? nullptr : &resource->second;
}

const char *ResourceRegistryStatusText(ResourceRegistryStatus status) noexcept
{
    switch (status)
    {
    case ResourceRegistryStatus::Accepted:
        return "accepted";
    case ResourceRegistryStatus::ConstructionEvidenceMissing:
        return "construction evidence is missing";
    case ResourceRegistryStatus::InvalidObject:
        return "resource object is invalid";
    case ResourceRegistryStatus::InvalidReferenceCount:
        return "initial reference count is invalid";
    case ResourceRegistryStatus::DuplicateObject:
        return "resource object was constructed twice";
    case ResourceRegistryStatus::UnknownObject:
        return "resource object is unknown";
    case ResourceRegistryStatus::ResourceTypeMismatch:
        return "resource type does not match construction";
    case ResourceRegistryStatus::BackingObjectMismatch:
        return "backing object does not match construction";
    case ResourceRegistryStatus::ReferenceCountMismatch:
        return "reference count does not match registry state";
    case ResourceRegistryStatus::ReferenceBoundary:
        return "reference boundary requires retained destructor semantics";
    }
    return "unknown";
}

} // namespace gears::native_rhi
