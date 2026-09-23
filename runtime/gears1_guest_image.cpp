#include "gears1_guest_image.h"

#include <algorithm>

#include <x360port/pe_image.hpp>
#include <x360port/xex_inspect.hpp>

namespace gears
{

bool Gears1GuestImage::Initialize(std::span<const std::byte> loaded_image,
                                  const XexIdentity &expected, std::span<const ImportSpec> imports,
                                  std::string &error)
{
    image_.clear();
    import_specs_.clear();
    imports_.clear();
    descriptor_ = {};

    if (loaded_image.size() != expected.imageSize ||
        x360port::HashBytes(loaded_image) != expected.imageDigest)
    {
        error = "loaded XEX image does not match the exact Gears profile";
        return false;
    }

    const x360port::PeImageLayoutResult loaded = x360port::DescribeLoadedPeImage(loaded_image);
    if (!loaded)
    {
        error = loaded.error;
        return false;
    }
    if (loaded.layout.identity.base != expected.imageBase ||
        loaded.layout.identity.entry_point != expected.entryPoint)
    {
        error = "loaded image geometry does not match the exact Gears profile";
        return false;
    }

    image_.assign(loaded_image.begin(), loaded_image.end());
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
    descriptor_.image = loaded.layout.identity;
    descriptor_.code = loaded.layout.code;
    RebuildManifest();
    error.clear();
    return true;
}

bool Gears1GuestImage::InitializeCheckedXex(std::span<const std::byte> xex,
                                            const XexIdentity &expected, std::string &error)
{
    if (x360port::HashBytes(xex) != expected.containerDigest)
    {
        error = "XEX container does not match the exact Gears profile";
        return false;
    }

    const x360port::XexInspectionResult inspected = x360port::InspectXex(xex);
    if (!inspected)
    {
        error = inspected.error;
        return false;
    }

    std::vector<ImportSpec> imports;
    imports.reserve(inspected.inspection.imports.size());
    for (const x360port::XexImport &import : inspected.inspection.imports)
    {
        imports.push_back({import.kind, import.library, import.ordinal, import.name, import.address,
                           import.record_address});
    }
    return Initialize(inspected.inspection.loaded_image, expected, imports, error);
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
