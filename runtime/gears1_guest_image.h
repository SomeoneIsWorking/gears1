#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <x360port/module_contract.hpp>

#include "title_profile.h"

namespace gears
{

struct ImportSpec
{
    x360port::ImportKind kind = x360port::ImportKind::Function;
    std::string library;
    std::uint32_t ordinal = 0;
    std::string name;
    x360port::GuestAddress address = 0;
    x360port::GuestAddress record_address = 0;
};

// Adapts the checked XEX inspector's normalized PE image to the flat image
// contract consumed by x360port. Profile authentication remains Gears-owned;
// this owner only maps the authenticated image and seals its runtime module.
class Gears1GuestImage final : public x360port::GuestModule
{
  public:
    Gears1GuestImage() = default;
    Gears1GuestImage(const Gears1GuestImage &) = delete;
    Gears1GuestImage &operator=(const Gears1GuestImage &) = delete;
    Gears1GuestImage(Gears1GuestImage &&) = delete;
    Gears1GuestImage &operator=(Gears1GuestImage &&) = delete;

    [[nodiscard]] bool Initialize(std::span<const std::byte> normalized_image,
                                  const XexIdentity &expected, std::span<const ImportSpec> imports,
                                  std::string &error);

    // Inspect and authenticate the user-owned XEX before adapting its
    // normalized image. The checked inspector owns container expansion and
    // import discovery; this title owner only verifies the exact profile and
    // converts the discovered manifest into x360port's module contract.
    [[nodiscard]] bool InitializeCheckedXex(std::span<const std::byte> xex,
                                            const XexIdentity &expected, std::string &error);

    [[nodiscard]] const x360port::ModuleDescriptor &Descriptor() const noexcept override;
    [[nodiscard]] std::span<const std::byte> ImageBytes() const noexcept override;
    [[nodiscard]] std::span<const x360port::ImportRequirement>
    ImportManifest() const noexcept override;

  private:
    void RebuildManifest();

    std::vector<std::byte> image_;
    std::vector<ImportSpec> import_specs_;
    std::vector<x360port::ImportRequirement> imports_;
    x360port::ModuleDescriptor descriptor_{};
};

} // namespace gears
