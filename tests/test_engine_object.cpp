// Synthetic contracts for the native engine's tagged-property streams:
// scalar, name and object values, script structs saved field by field, and
// arrays of such structs, with refusal of bytes a stream does not account for.

#include <cassert>
#include <cstdint>
#include <string_view>

#include "engine_package_fixture.h"
#include "object/property_values.h"
#include "object/tagged_properties.h"
#include "package/byte_reader.h"
#include "package/package.h"

namespace
{

using gears::engine::object::PropertyValues;
using gears::engine::object::TaggedProperties;
using gears::engine::package::Package;
using gears::engine::package::PackageFormatError;
using gears::engine::test::Bytes;
using gears::engine::test::PlainPackage;
using gears::engine::test::Writer;

// Name indices after the fixture's Core, Package, Thing.
enum Name : std::uint8_t
{
    kThing = 2,
    kNone,
    kIntProperty,
    kStructProperty,
    kArrayProperty,
    kNameProperty,
    kObjectProperty,
    kCount,
    kInput,
    kExpressionInput,
    kParams,
    kParameterName,
    kExpression,
    kDiffuse,
};

Package TestPackage()
{
    return Package::Load(
        "Test", PlainPackage(gears::engine::package::kGears1PackageFileVersion, 2,
                             {"None", "IntProperty", "StructProperty", "ArrayProperty",
                              "NameProperty", "ObjectProperty", "Count", "Input", "ExpressionInput",
                              "Params", "ParameterName", "Expression", "Diffuse"}));
}

void Tag(Writer &w, Name name, Name type, std::size_t size)
{
    w.Name(name);
    w.Name(type);
    w.U32(static_cast<std::uint32_t>(size));
    w.U32(0);
}

void Append(Writer &w, const Bytes &bytes)
{
    w.bytes.insert(w.bytes.end(), bytes.begin(), bytes.end());
}

// An ExpressionInput struct whose Expression is export 1.
Bytes ExpressionInput()
{
    Writer w;
    Tag(w, kExpression, kObjectProperty, 4);
    w.U32(1);
    w.Name(kNone);
    return w.bytes;
}

// Two struct elements: {ParameterName = Thing} and {Count = 7}.
Bytes Params()
{
    Writer w;
    w.U32(2);
    Tag(w, kParameterName, kNameProperty, 8);
    w.Name(kThing);
    w.Name(kNone);
    Tag(w, kCount, kIntProperty, 4);
    w.U32(7);
    w.Name(kNone);
    return w.bytes;
}

template <typename Action> bool Refuses(Action action)
{
    try
    {
        action();
    }
    catch (const PackageFormatError &)
    {
        return true;
    }
    return false;
}

void TestNestedStreams()
{
    Package package = TestPackage();
    Bytes input = ExpressionInput();
    Bytes params = Params();
    Writer w;
    Tag(w, kInput, kStructProperty, input.size());
    w.Name(kExpressionInput);
    Append(w, input);
    Tag(w, kParams, kArrayProperty, params.size());
    Append(w, params);
    w.Name(kNone);

    TaggedProperties stream = TaggedProperties::Parse(w.bytes, package);
    assert(stream.size() == 2U);
    PropertyValues values(stream);
    auto fields = values.StructFields("Input", "ExpressionInput");
    assert(fields.has_value());
    assert(PropertyValues(*fields).Object("Expression") == 1);
    assert(!values.StructFields("Diffuse", "ExpressionInput").has_value());
    assert(Refuses([&] { (void)values.StructFields("Input", "Params"); }));

    auto elements = values.StructArray("Params");
    assert(elements.size() == 2U);
    assert(PropertyValues(elements[0]).Name("ParameterName") == "Thing");
    assert(PropertyValues(elements[1]).Int("Count") == 7);
    assert(!PropertyValues(elements[1]).Name("ParameterName").has_value());
    assert(Refuses([&] { (void)PropertyValues(elements[0]).Int("ParameterName"); }));
}

void TestRefusals()
{
    Package package = TestPackage();
    Bytes trailing = ExpressionInput();
    trailing.push_back(0);
    assert(Refuses([&] { (void)TaggedProperties::Parse(trailing, package); }));
    Bytes unterminated = ExpressionInput();
    unterminated.resize(unterminated.size() - 8U);
    assert(Refuses([&] { (void)TaggedProperties::Parse(unterminated, package); }));
    Bytes extra_element = Params();
    extra_element.push_back(0);
    assert(Refuses([&] { (void)TaggedProperties::ParseArray(extra_element, package); }));
    Bytes overcounted = Params();
    overcounted[3] = 3;
    assert(Refuses([&] { (void)TaggedProperties::ParseArray(overcounted, package); }));
}

} // namespace

int main()
{
    TestNestedStreams();
    TestRefusals();
    return 0;
}
