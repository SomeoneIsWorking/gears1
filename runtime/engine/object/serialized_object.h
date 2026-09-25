#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "class_hierarchy.h"
#include "tagged_properties.h"
#include "package/object_tables.h"
#include "package/package.h"

namespace gears::engine::object
{

using package::NameReference;
using package::Package;
using package::PackageIndex;

// Export object flag: the object is its class's default object.
inline constexpr std::uint64_t kObjectFlagClassDefaultObject = 0x0000000000000200ULL;
// Export object flag: the object's data starts with a script state frame.
inline constexpr std::uint64_t kObjectFlagHasStack = 0x0200000000000000ULL;

// Root class of subobjects that save their template owner class ahead of
// their net index (class default objects excepted).
inline constexpr std::string_view kComponentClass = "Core.Component";

// The script execution state an actor saves ahead of its properties.
struct StateFrame
{
    PackageIndex node = 0;
    PackageIndex state_node = 0;
    std::uint64_t probe_mask = 0;
    std::uint32_t latent_action = 0;
    std::int32_t code_offset = 0;
};

// The generic part of one export's data: the optional state frame, a
// component's template owner class, the net index, and the tagged properties
// up to the terminating "None". Everything
// after `native_offset` belongs to the class's native serializer.
class SerializedObject
{
  public:
    // Reads export `export_index` (zero-based) of `package`. Refuses class,
    // struct, and field exports, whose data is a schema rather than a
    // property stream.
    static SerializedObject Read(const Package &package, std::size_t export_index,
                                 ClassHierarchy &classes);

    [[nodiscard]] const std::optional<StateFrame> &State() const noexcept { return state_; }
    [[nodiscard]] std::int32_t NetIndex() const noexcept { return net_index_; }
    // A class default object holds only defaults; it carries no native data.
    [[nodiscard]] bool IsClassDefault() const noexcept { return class_default_; }
    // A component's template owner class, or 0 for none or a non-component.
    [[nodiscard]] PackageIndex TemplateOwnerClass() const noexcept { return template_owner_class_; }
    [[nodiscard]] const TaggedProperties &Properties() const noexcept { return properties_; }
    [[nodiscard]] std::size_t NativeOffset() const noexcept { return native_offset_; }
    [[nodiscard]] std::span<const std::uint8_t> NativeData() const noexcept
    {
        return data_.subspan(native_offset_);
    }
    // Absolute package offset of `NativeData()`'s first byte.
    [[nodiscard]] std::size_t NativeBase() const noexcept { return data_base_ + native_offset_; }
    [[nodiscard]] const Package &Owner() const noexcept { return *package_; }
    // This object's package index (one-based export index).
    [[nodiscard]] PackageIndex Index() const noexcept { return index_; }

  private:
    SerializedObject(const Package &package, PackageIndex index, std::span<const std::uint8_t> data,
                     std::size_t data_base)
        : package_(&package), index_(index), data_(data), data_base_(data_base),
          properties_(package)
    {
    }

    const Package *package_;
    PackageIndex index_;
    std::span<const std::uint8_t> data_;
    std::size_t data_base_;
    std::optional<StateFrame> state_;
    bool class_default_ = false;
    PackageIndex template_owner_class_ = 0;
    NameReference template_name_;
    std::int32_t net_index_ = 0;
    TaggedProperties properties_;
    std::size_t native_offset_ = 0;
};

// True when an export's class makes its data a schema (a class, struct,
// function, state, enum, constant, property, or script text) rather than a
// property stream.
bool IsSchemaExport(const Package &package, std::size_t export_index);

// True when an export or any export in its outer chain is a class default
// object: the object is then part of a class's defaults (a template).
bool IsInsideClassDefaults(const Package &package, std::size_t export_index);

} // namespace gears::engine::object
