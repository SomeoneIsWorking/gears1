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
            object.template_name_ = package::ReadNameReference(reader);
        }
    }
    object.net_index_ = reader.ReadI32();
    object.properties_ = TaggedProperties::Read(reader, package);
    object.native_offset_ = reader.Offset();
    return object;
}

} // namespace gears::engine::object
