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

bool SameIdentity(const ResourceRecord &record, const RhiResourceIdentityEvidence &identity)
{
    return record.object == identity.object && record.objectFlags == identity.rawFlags &&
           record.resourceType == identity.resourceType &&
           record.backingObject == identity.backingObject;
}

} // namespace

ResourceRegistryResult ResourceRegistry::AdoptExisting(const RhiResourceIdentityEvidence &identity)
{
    if (!identity.present)
        return Result(ResourceRegistryStatus::AdoptionEvidenceMissing);
    if (identity.object == 0)
        return Result(ResourceRegistryStatus::InvalidObject);
    if (identity.referenceCount == 0)
        return Result(ResourceRegistryStatus::InvalidReferenceCount);
    if (identity.resourceType != (identity.rawFlags & kResourceTypeMask))
        return Result(ResourceRegistryStatus::ResourceTypeMismatch);

    const auto existing = resources_.find(identity.object);
    if (existing != resources_.end())
    {
        if (!SameIdentity(existing->second, identity))
            return Result(ResourceRegistryStatus::IdentityMismatch,
                          existing->second.referenceCount);
        if (existing->second.referenceCount != identity.referenceCount)
            return Result(ResourceRegistryStatus::ReferenceCountMismatch,
                          existing->second.referenceCount);
        return Result(ResourceRegistryStatus::Accepted, existing->second.referenceCount);
    }

    resources_.emplace(identity.object, ResourceRecord{.object = identity.object,
                                                       .objectFlags = identity.rawFlags,
                                                       .resourceType = identity.resourceType,
                                                       .backingObject = identity.backingObject,
                                                       .referenceCount = identity.referenceCount});
    return Result(ResourceRegistryStatus::Accepted, identity.referenceCount);
}

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
                                      .objectWords = evidence.objectWords,
                                      .constructionObserved = true});
    return Result(ResourceRegistryStatus::Accepted, evidence.initialReferenceCount);
}

ResourceRegistryResult ResourceRegistry::ApplyLifetime(const ResourceLifetimeCommand &command)
{
    const RhiSemanticResourceLifetime &lifetime = command.lifetime;
    if (command.identity.present &&
        (command.identity.object != lifetime.object ||
         command.identity.rawFlags != lifetime.rawFlags ||
         command.identity.resourceType != lifetime.resourceType ||
         command.identity.backingObject != lifetime.backingObject ||
         command.identity.referenceCount != lifetime.previousReferenceCount))
        return Result(ResourceRegistryStatus::IdentityMismatch);

    auto resource = resources_.find(lifetime.object);
    if (resource == resources_.end() && command.identity.present)
    {
        if ((lifetime.operation == RhiResourceLifetimeOperation::AddReference &&
             lifetime.previousReferenceCount == 0) ||
            (lifetime.operation == RhiResourceLifetimeOperation::Release &&
             lifetime.previousReferenceCount <= 1))
            return Result(ResourceRegistryStatus::ReferenceBoundary);
        const ResourceRegistryResult adopted = AdoptExisting(command.identity);
        if (adopted.status != ResourceRegistryStatus::Accepted)
            return adopted;
        resource = resources_.find(lifetime.object);
    }
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
    case ResourceRegistryStatus::AdoptionEvidenceMissing:
        return "pre-existing resource identity evidence is missing";
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
    case ResourceRegistryStatus::IdentityMismatch:
        return "pre-existing resource identity does not match registry state";
    case ResourceRegistryStatus::ReferenceBoundary:
        return "reference boundary requires retained destructor semantics";
    }
    return "unknown";
}

} // namespace gears::native_rhi
