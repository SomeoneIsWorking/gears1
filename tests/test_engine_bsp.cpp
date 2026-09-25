// BSP triangulation of the native engine: a model component's nodes become
// fans with one section per element and texture coordinates projected on
// their surface's axes.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "bsp/bsp_geometry.h"
#include "bsp/bsp_model.h"
#include "bsp/model_component.h"
#include "package/byte_reader.h"

namespace
{

using gears::engine::bsp::BspModel;
using gears::engine::bsp::BspNode;
using gears::engine::bsp::BspSurface;
using gears::engine::bsp::BspVertex;
using gears::engine::bsp::LightMap2D;
using gears::engine::bsp::ModelComponent;
using gears::engine::bsp::TriangulateComponent;
using gears::engine::mesh::Vector3;

constexpr float kTolerance = 1e-5F;
constexpr std::int32_t kFloorMaterial = -3;
constexpr std::int32_t kWallMaterial = 7;

bool Near(float a, float b)
{
    return std::fabs(a - b) < kTolerance;
}

// A floor quad (node 0) and a wall triangle (node 1), each with its own
// surface; the floor's texture axes are X and Y from the origin.
BspModel FloorAndWall()
{
    std::vector<Vector3> vectors{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    std::vector<Vector3> points{{0.0F, 0.0F, 0.0F},
                                {256.0F, 0.0F, 0.0F},
                                {256.0F, 128.0F, 0.0F},
                                {0.0F, 128.0F, 0.0F},
                                {0.0F, 0.0F, 64.0F}};
    std::vector<BspSurface> surfaces{
        {kFloorMaterial, 0U, 0, 2, 0, 1},
        {kWallMaterial, 0U, 0, 1, 0, 2},
    };
    std::vector<BspNode> nodes{
        {0.0F, 0.0F, 1.0F, 0.0F, 0, 0, 4},
        {0.0F, -1.0F, 0.0F, 0.0F, 4, 1, 3},
    };
    std::vector<BspVertex> vertices{{0, {0.0F, 0.0F}}, {1, {1.0F, 0.0F}}, {2, {1.0F, 1.0F}},
                                    {3, {0.0F, 1.0F}}, {0, {0.0F, 0.0F}}, {1, {1.0F, 0.0F}},
                                    {4, {0.0F, 1.0F}}};
    return {std::move(vectors), std::move(points), std::move(nodes), std::move(surfaces),
            std::move(vertices)};
}

void TestTriangulation()
{
    BspModel model = FloorAndWall();
    ModelComponent component(1, {{kWallMaterial, {1}}, {kFloorMaterial, {0}}});
    auto mesh = TriangulateComponent(model, component);
    const auto &lod = mesh.lod;
    assert(mesh.light_maps.size() == 2U && !mesh.light_maps[0] && !mesh.light_maps[1]);
    // Sections follow the component's elements, not the model's node order.
    assert(lod.sections.size() == 2U);
    assert(lod.sections[0].material == kWallMaterial);
    assert(lod.sections[0].first_index == 0U && lod.sections[0].triangle_count == 1U);
    assert(lod.sections[1].material == kFloorMaterial);
    assert(lod.sections[1].first_index == 3U && lod.sections[1].triangle_count == 2U);
    assert(lod.vertices.size() == 7U);
    assert((lod.indices == std::vector<std::uint16_t>{0, 1, 2, 3, 4, 5, 3, 5, 6}));
    // The floor's far corner is two repeats along U and one along V.
    const auto &corner = lod.vertices[5];
    assert(Near(corner.uv[0][0], 2.0F) && Near(corner.uv[0][1], 1.0F));
    assert(Near(corner.normal.z, 1.0F));
    // The wall's top vertex lies half a repeat up its V axis.
    assert(Near(lod.vertices[2].uv[0][1], 0.5F) && Near(lod.vertices[2].normal.y, -1.0F));
}

void TestElementWithoutTrianglesIsDropped()
{
    ModelComponent component(1, {{kWallMaterial, {}}, {kFloorMaterial, {0}}});
    auto mesh = TriangulateComponent(FloorAndWall(), component);
    assert(mesh.lod.sections.size() == 1U && mesh.lod.sections[0].material == kFloorMaterial);
    assert(mesh.light_maps.size() == 1U);
}

void TestLightMapCoordinates()
{
    LightMap2D light_map;
    light_map.coordinate_scale = {0.5F, 0.25F};
    light_map.coordinate_bias = {0.125F, 0.5F};
    ModelComponent component(1, {{kFloorMaterial, {0}, light_map}});
    auto mesh = TriangulateComponent(FloorAndWall(), component);
    assert(mesh.light_maps.size() == 1U && mesh.light_maps[0]);
    // The floor's far corner stores shadow coordinate (1, 1).
    const auto &corner = mesh.lod.vertices[2];
    assert(Near(corner.uv[1][0], 0.625F) && Near(corner.uv[1][1], 0.75F));
    assert(mesh.lod.tex_coord_count == 2U);
}

void TestRefusesNodeOutsideModel()
{
    ModelComponent component(1, {{kFloorMaterial, {2}}});
    bool refused = false;
    try
    {
        (void)TriangulateComponent(FloorAndWall(), component);
    }
    catch (const gears::engine::package::PackageFormatError &)
    {
        refused = true;
    }
    assert(refused);
}

void TestModelRefusesVertexOutsidePoints()
{
    bool refused = false;
    try
    {
        // One triangle whose last vertex names a point past the only one.
        BspModel model({{1.0F, 0.0F, 0.0F}}, {{0.0F, 0.0F, 0.0F}},
                       {{0.0F, 0.0F, 1.0F, 0.0F, 0, 0, 3}}, {{kFloorMaterial, 0U, 0, 0, 0, 0}},
                       {{0, {}}, {0, {}}, {1, {}}});
    }
    catch (const gears::engine::package::PackageFormatError &)
    {
        refused = true;
    }
    assert(refused);
}

} // namespace

int main()
{
    TestTriangulation();
    TestElementWithoutTrianglesIsDropped();
    TestLightMapCoordinates();
    TestRefusesNodeOutsideModel();
    TestModelRefusesVertexOutsidePoints();
    return 0;
}
