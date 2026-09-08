#include "gears1_guest_image.h"

#include <algorithm>

#include <x360port/pe_image.hpp>

namespace gears
{

bool Gears1GuestImage::Initialize(std::span<const std::byte> normalized_image,
                                  const XexIdentity &expected, std::span<const ImportSpec> imports,
                                  std::string &error)
{
    image_.clear();
    import_specs_.clear();
    imports_.clear();
    descriptor_ = {};

    if (normalized_image.size() != expected.imageSize ||
        x360port::HashBytes(normalized_image) != expected.imageDigest)
    {
        error = "normalized XEX image does not match the exact Gears profile";
        return false;
    }

    const x360port::PeImageLayoutResult mapped = x360port::MapPeImage(normalized_image);
    if (!mapped)
    {
        error = mapped.error;
        return false;
    }
    if (mapped.layout.identity.base != expected.imageBase ||
        mapped.layout.identity.entry_point != expected.entryPoint)
    {
        error = "normalized image geometry does not match the exact Gears profile";
        return false;
    }

    image_ = mapped.layout.image;
    import_specs_.assign(imports.begin(), imports.end());
    std::ranges::sort(import_specs_,
                      [](const ImportSpec &left, const ImportSpec &right)
                      {
                          if (left.library != right.library)
                          {
                              return left.library < right.library;
                          }
                          return left.ordinal < right.ordinal;
                      });
    descriptor_.image = mapped.layout.identity;
    descriptor_.code = mapped.layout.code;
    RebuildManifest();
    error.clear();
    return true;
}

const x360port::ModuleDescriptor &Gears1GuestImage::Descriptor() const noexcept
{
    return descriptor_;
}

std::span<const std::byte> Gears1GuestImage::ImageBytes() const noexcept
{
    return image_;
}

std::span<const x360port::ImportRequirement> Gears1GuestImage::ImportManifest() const noexcept
{
    return imports_;
}

void Gears1GuestImage::RebuildManifest()
{
    imports_.clear();
    imports_.reserve(import_specs_.size());
    for (const ImportSpec &import : import_specs_)
    {
        imports_.push_back({import.kind, import.library, import.ordinal, import.name,
                            import.address, import.record_address});
    }
    descriptor_.import_count = imports_.size();
    descriptor_.import_manifest_sha256 = x360port::HashImportManifest(imports_);
}

} // namespace gears
