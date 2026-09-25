#pragma once

#include <string>

#include "render/vulkan_device.h"

struct SDL_Window;

namespace gears::engine::app
{

// The game's desktop window: SDL's video and gamepad subsystems, one
// resizable Vulkan window with the mouse captured for looking, and the
// Vulkan surface request that presents into it.
class GameWindow
{
  public:
    // Refuses when SDL or the window cannot be created.
    GameWindow(const std::string &title, VkExtent2D size);
    ~GameWindow();
    GameWindow(const GameWindow &) = delete;
    GameWindow &operator=(const GameWindow &) = delete;

    [[nodiscard]] render::SurfaceRequest Surface() const;
    // The window's size in pixels.
    [[nodiscard]] VkExtent2D Drawable() const;
    // Captures the mouse for looking, or releases it to the desktop.
    void CaptureMouse(bool captured) const;

  private:
    SDL_Window *window_ = nullptr;
};

} // namespace gears::engine::app
