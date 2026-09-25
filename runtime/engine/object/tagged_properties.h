#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "package/byte_reader.h"
#include "package/object_tables.h"
#include "package/package.h"

namespace gears::engine::object
{

using package::NameReference;
using package::Package;
using package::PackageIndex;

// One tagged property: its name, its type name, the element of a static
// array it fills, the struct type of a struct value, and its encoded value.
// A boolean's value lives in the tag and its value span is empty.
struct PropertyTag
{
    NameReference name;
    NameReference type;
    std::int32_t array_index = 0;
    NameReference struct_name;
    bool bool_value = false;
    std::span<const std::uint8_t> value;
};

// A stream of tagged properties ended by the name "None": an object's
// properties, or the value of a script struct saved field by field.
class TaggedProperties
{
  public:
    // Reads tags from `reader` through the terminating "None"; names resolve
    // in `package`.
    static TaggedProperties Read(package::ByteReader &reader, const Package &package);
    // A struct value saved as tags; refuses bytes after its "None".
    static TaggedProperties Parse(std::span<const std::uint8_t> bytes, const Package &package);
    // An array value whose elements are structs saved as tags: a count, then
    // each element's stream. Refuses bytes after the last element.
    static std::vector<TaggedProperties> ParseArray(std::span<const std::uint8_t> bytes,
                                                    const Package &package);

    // An empty stream of `package`.
    explicit TaggedProperties(const Package &package) : package_(&package) {}

    [[nodiscard]] const Package &Owner() const noexcept { return *package_; }
    // The first tag with this name and array index, or none.
    [[nodiscard]] const PropertyTag *Find(std::string_view name,
                                          std::int32_t array_index = 0) const;

    [[nodiscard]] std::size_t size() const noexcept { return tags_.size(); }
    [[nodiscard]] auto begin() const noexcept { return tags_.begin(); }
    [[nodiscard]] auto end() const noexcept { return tags_.end(); }

  private:
    const Package *package_;
    std::vector<PropertyTag> tags_;
};

} // namespace gears::engine::object
