#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include "tagged_properties.h"

namespace gears::engine::object
{

// Typed reads of the named properties of one tagged stream (an object's, or
// a struct value's). Each refuses a property whose tag type or size does not
// match, and returns the default when the stream does not store the property
// (it keeps its class or struct default).
class PropertyValues
{
  public:
    explicit PropertyValues(const TaggedProperties &properties) : properties_(properties) {}

    [[nodiscard]] std::int32_t Int(std::string_view name, std::int32_t fallback = 0) const;
    [[nodiscard]] float Float(std::string_view name, float fallback = 0.0F) const;
    [[nodiscard]] std::uint8_t Byte(std::string_view name, std::uint8_t fallback = 0) const;
    [[nodiscard]] bool Bool(std::string_view name, bool fallback = false) const;
    [[nodiscard]] PackageIndex Object(std::string_view name) const;
    // A name property's text, or none when absent.
    [[nodiscard]] std::optional<std::string> Name(std::string_view name) const;
    // The encoded value of a struct property of type `struct_name`, or an
    // empty span when the object does not store it. Refuses another type or a
    // value of a size other than `size`.
    [[nodiscard]] std::span<const std::uint8_t>
    Struct(std::string_view name, std::string_view struct_name, std::size_t size) const;
    // An array property's element count and encoded elements (each
    // `element_size` bytes); an empty span when absent.
    [[nodiscard]] std::span<const std::uint8_t> Array(std::string_view name,
                                                      std::size_t element_size) const;
    // An array of object references; empty when absent.
    [[nodiscard]] std::vector<PackageIndex> ObjectArray(std::string_view name) const;
    // A script struct saved field by field, or none when absent. Refuses
    // another struct type.
    [[nodiscard]] std::optional<TaggedProperties> StructFields(std::string_view name,
                                                               std::string_view struct_name) const;
    // An array of script structs saved field by field; empty when absent.
    [[nodiscard]] std::vector<TaggedProperties> StructArray(std::string_view name) const;

  private:
    [[nodiscard]] const PropertyTag *Tag(std::string_view name, std::string_view type,
                                         std::size_t size) const;

    const TaggedProperties &properties_;
};

} // namespace gears::engine::object
