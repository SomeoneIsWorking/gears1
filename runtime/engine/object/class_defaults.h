#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "class_hierarchy.h"
#include "object_resolver.h"
#include "serialized_object.h"

namespace gears::engine::object
{

// The defaults an object of one class starts from: its class's default
// object and those of every superclass, most derived first. A property reads
// from the most derived default that stores it; a class stores only the
// properties it changes from its superclass.
class DefaultChain
{
  public:
    explicit DefaultChain(std::vector<SerializedObject> defaults) : defaults_(std::move(defaults))
    {
    }

    [[nodiscard]] bool Empty() const noexcept { return defaults_.empty(); }
    // True when some default in the chain stores `name`.
    [[nodiscard]] bool Has(std::string_view name) const { return Storing(name) != nullptr; }

    [[nodiscard]] std::int32_t Int(std::string_view name, std::int32_t fallback = 0) const;
    [[nodiscard]] float Float(std::string_view name, float fallback = 0.0F) const;
    [[nodiscard]] bool Bool(std::string_view name, bool fallback = false) const;
    // The full path ("Package.Outer.Name") of an object property's value, or
    // none when no default stores it or it is None.
    [[nodiscard]] std::optional<std::string> ObjectPath(std::string_view name) const;
    // The encoded value of a struct property (see PropertyValues::Struct),
    // or an empty span when no default stores it.
    [[nodiscard]] std::span<const std::uint8_t>
    Struct(std::string_view name, std::string_view struct_name, std::size_t size) const;

  private:
    // The most derived default that stores `name`, or none.
    [[nodiscard]] const SerializedObject *Storing(std::string_view name) const;

    std::vector<SerializedObject> defaults_;
};

// Class default objects ("Default__<Class>" in the class's package) and the
// default subobjects they own ("Default__<Class>.<Name>"), read on first
// use and cached by class path.
class ClassDefaults
{
  public:
    ClassDefaults(ClassHierarchy &classes, ObjectResolver &resolver)
        : classes_(classes), resolver_(resolver)
    {
    }

    // The defaults of `class_path` ("Package.Class"). Refuses a class whose
    // chain names no default object at all.
    [[nodiscard]] const DefaultChain &Of(const std::string &class_path);
    // The defaults of the subobject `name` that `class_path`'s default
    // object owns, inherited through the same-named subobjects of its
    // superclasses' defaults. Refuses a subobject no class in the chain
    // defines.
    [[nodiscard]] const DefaultChain &Subobject(const std::string &class_path,
                                                std::string_view name);

  private:
    // Reads, for `class_path` and each superclass, the export at
    // "Default__<Class>" plus `suffix`, skipping classes that define none.
    [[nodiscard]] DefaultChain Read(const std::string &class_path, std::string_view suffix);

    ClassHierarchy &classes_;
    ObjectResolver &resolver_;
    std::map<std::pair<std::string, std::string>, DefaultChain, std::less<>> chains_;
};

} // namespace gears::engine::object
