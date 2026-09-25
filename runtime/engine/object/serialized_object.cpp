#include "serialized_object.h"

#include <algorithm>
#include <array>
#include <format>

#include "package/byte_reader.h"

namespace gears::engine::object
{
namespace
{

using package::ByteOrder;
using package::ByteReader;

// Class names whose instances are script schema, not property streams.
constexpr std::array<std::string_view, 21> kSchemaClasses{"Class",
                                                          "ScriptStruct",
                                                          "Struct",
                                                          "Function",
                                                          "State",
                                                          "Enum",
                                                          "Const",
                                                          "TextBuffer",
                                                          "ByteProperty",
                                                          "IntProperty",
                                                          "BoolProperty",
                                                          "FloatProperty",
                                                          "ObjectProperty",
                                                          "ClassProperty",
                                                          "NameProperty",
                                                          "StrProperty",
                                                          "StructProperty",
                                                          "ArrayProperty",
                                                          "MapProperty",
                                                          "DelegateProperty",
                                                          "ComponentProperty"};

NameReference ReadName(ByteReader &reader)
{
    NameReference name;
    name.index = reader.ReadU32();
    name.number = reader.ReadU32();
    return name;
}

StateFrame ReadStateFrame(ByteReader &reader)
{
    StateFrame frame;
    frame.node = reader.ReadI32();
    frame.state_node = reader.ReadI32();
    frame.probe_mask = reader.ReadU64();
    frame.latent_action = reader.ReadU32();
    std::size_t pushed_states = reader.ReadCount(0);
    if (pushed_states != 0U)
    {
        reader.Fail(std::format("state frame holds {} pushed state(s); their layout is not "
                                "measured",
                                pushed_states));
    }
    frame.code_offset = reader.ReadI32();
    return frame;
}

} // namespace

bool IsInsideClassDefaults(const Package &package, std::size_t export_index)
{
    const auto &exports = package.Tables().exports;
    PackageIndex current = static_cast<PackageIndex>(export_index + 1U);
    for (std::size_t steps = 0; current > 0 && steps <= exports.size(); ++steps)
    {
        const package::ObjectExport &object = exports[static_cast<std::size_t>(current) - 1U];
        if ((object.object_flags & kObjectFlagClassDefaultObject) != 0U)
        {
            return true;
        }
        current = object.outer;
    }
    return false;
}

bool IsSchemaExport(const Package &package, std::size_t export_index)
{
    std::string class_name = package.ClassName(static_cast<PackageIndex>(export_index + 1U));
    return std::ranges::find(kSchemaClasses, class_name) != kSchemaClasses.end();
}

SerializedObject SerializedObject::Read(const Package &package, std::size_t export_index,
                                        ClassHierarchy &classes)
{
    if (IsSchemaExport(package, export_index))
    {
        throw package::PackageFormatError(
            std::format("export {} is a schema object, not a property stream", export_index + 1U));
    }
    SerializedObject object(package, static_cast<PackageIndex>(export_index + 1U),
                            package.ExportData(export_index),
                            package.Tables().exports[export_index].serial_offset);
    ByteReader reader(object.data_, ByteOrder::Big);
    if ((package.Tables().exports[export_index].object_flags & kObjectFlagHasStack) != 0U)
    {
        object.state_ = ReadStateFrame(reader);
    }
    std::string class_path =
        ClassHierarchy::ClassPath(package, static_cast<PackageIndex>(export_index + 1U));
    std::uint64_t flags = package.Tables().exports[export_index].object_flags;
    object.class_default_ = (flags & kObjectFlagClassDefaultObject) != 0U;
    if ((flags & kObjectFlagClassDefaultObject) == 0U && classes.IsA(class_path, kComponentClass))
    {
        object.template_owner_class_ = reader.ReadI32();
        // A component template inside class defaults also names itself.
        if (IsInsideClassDefaults(package, export_index))
        {
            object.template_name_ = ReadName(reader);
        }
    }
    object.net_index_ = reader.ReadI32();
    while (true)
    {
        PropertyTag tag;
        tag.name = ReadName(reader);
        std::string name = package.NameText(tag.name);
        if (name == "None")
        {
            break;
        }
        tag.type = ReadName(reader);
        std::string type = package.NameText(tag.type);
        std::size_t size = reader.ReadCount(1);
        tag.array_index = reader.ReadI32();
        if (type == "StructProperty")
        {
            tag.struct_name = ReadName(reader);
            if (!package.HasName(tag.struct_name))
            {
                reader.Fail(std::format("struct '{}' names entry {} of {}", name,
                                        tag.struct_name.index, package.Tables().names.size()));
            }
        }
        else if (type == "BoolProperty")
        {
            std::uint32_t value = reader.ReadU32();
            if (value > 1U || size != 0U)
            {
                reader.Fail(
                    std::format("boolean '{}' has value {} and size {}", name, value, size));
            }
            tag.bool_value = value != 0U;
        }
        tag.value = reader.ReadBytes(size);
        object.properties_.push_back(tag);
    }
    object.native_offset_ = reader.Offset();
    return object;
}

const PropertyTag *SerializedObject::Find(std::string_view name, std::int32_t array_index) const
{
    for (const PropertyTag &tag : properties_)
    {
        if (tag.array_index == array_index && package_->NameText(tag.name) == name)
        {
            return &tag;
        }
    }
    return nullptr;
}

} // namespace gears::engine::object
