// gears_native: Gears of War run by the native engine. Loads a persistent
// level and its streamed sublevels from the cooked content directory, spawns
// the player at the level's PlayerStart, and runs the game loop in a window.

#include <SDL3/SDL.h>
#include <exception>
#include <filesystem>
#include <string>

#include <lucent/log.h>

#include "device_input.h"
#include "game/game_world.h"
#include "game_window.h"
#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "package/content_files.h"
#include "package/package_store.h"
#include "render/level_renderer.h"
#include "render/vulkan_device.h"
#include "render/window_frames.h"
#include "scene/static_meshes.h"
#include "scene/world.h"

namespace
{

namespace fs = std::filesystem;
namespace engine = gears::engine;

// The resolution the game renders at; the window shows it letterboxed.
constexpr VkExtent2D kRenderExtent{1920, 1080};
constexpr VkExtent2D kWindowExtent{1600, 900};
constexpr double kNanosecondsPerSecond = 1e9;

void RunGame(const fs::path &content, const std::string &level_name)
{
    engine::package::ContentFiles files(content);
    engine::package::PackageStore store(files);
    engine::object::ClassHierarchy classes(store);
    engine::object::ObjectResolver resolver(store);
    auto world = engine::scene::World::Load(store, classes, resolver, level_name);
    engine::scene::StaticMeshes static_meshes(classes);
    engine::game::GameWorld game(world, static_meshes, engine::game::PawnTuning{},
                                 engine::game::CameraRig{});
    lucent::info("gears-native", "{}: {} level(s), {} collision triangle(s)",
                 world.Persistent().Name(), world.Levels().size(),
                 game.Collision().TriangleCount());

    engine::app::GameWindow window("Gears of War", kWindowExtent);
    engine::render::VulkanDevice device(window.Surface());
    engine::render::LevelRenderer renderer(device, kRenderExtent, files, classes, resolver,
                                           static_meshes);
    for (const auto &level : world.Levels())
    {
        renderer.Prepare(*level.package, level.scene);
    }
    engine::render::WindowFrames frames(device, window.Drawable());
    engine::app::DeviceInput input(engine::app::InputSettings{});
    lucent::info("gears-native", "{} section draw(s) on {}", renderer.Census().draws,
                 device.Name());

    float aspect =
        static_cast<float>(kRenderExtent.width) / static_cast<float>(kRenderExtent.height);
    Uint64 previous = SDL_GetTicksNS();
    while (input.Poll())
    {
        Uint64 now = SDL_GetTicksNS();
        auto seconds =
            static_cast<float>(static_cast<double>(now - previous) / kNanosecondsPerSecond);
        previous = now;
        game.Advance(input.Read(seconds), seconds);
        engine::scene::Camera view = game.View(aspect);
        frames.Draw(
            window.Drawable(), [&](VkCommandBuffer commands) { renderer.Record(commands, view); },
            renderer.ColorImage(), kRenderExtent);
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        lucent::error("gears-native",
                      "usage: gears_native <cooked content directory> <persistent level>");
        return 2;
    }
    try
    {
        RunGame(argv[1], argv[2]);
    }
    catch (const std::exception &error)
    {
        lucent::error("gears-native", "{}", error.what());
        return 1;
    }
    return 0;
}
