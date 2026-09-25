#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "serialized_object.h"

namespace gears::engine::object
{

// Typed reads of one object's named scalar properties. Each refuses a
// property whose tag type or size does not match, and returns the default
// when the object does not store the property (it keeps its class default).
class PropertyValues
{
  public:
    explicit PropertyValues(const SerializedObject &object) : object_(object) {}

    [[nodiscard]] std::int32_t Int(std::string_view name, std::int32_t fallback = 0) const;
    [[nodiscard]] float Float(std::string_view name, float fallback = 0.0F) const;
    [[nodiscard]] std::uint8_t Byte(std::string_view name, std::uint8_t fallback = 0) const;
    [[nodiscard]] bool Bool(std::string_view name, bool fallback = false) const;
    [[nodiscard]] PackageIndex Object(std::string_view name) const;
    // The encoded value of a struct property of type `struct_name`, or an
    // empty span when the object does not store it. Refuses another type or a
    // value of a size other than `size`.
    [[nodiscard]] std::span<const std::uint8_t>
    Struct(std::string_view name, std::string_view struct_name, std::size_t size) const;
    // An array property's element count and encoded elements (each
    // `element_size` bytes); an empty span when absent.
    [[nodiscard]] std::span<const std::uint8_t> Array(std::string_view name,
                                                      std::size_t element_size) const;

  private:
    [[nodiscard]] const PropertyTag *Tag(std::string_view name, std::string_view type,
                                         std::size_t size) const;

    const SerializedObject &object_;
};

} // namespace gears::engine::object
