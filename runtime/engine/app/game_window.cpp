#include "game_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdexcept>

namespace gears::engine::app
{

GameWindow::GameWindow(const std::string &title, VkExtent2D size)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    }
    window_ =
        SDL_CreateWindow(title.c_str(), static_cast<int>(size.width), static_cast<int>(size.height),
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr)
    {
        std::string reason = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL_CreateWindow: " + reason);
    }
    CaptureMouse(true);
}

GameWindow::~GameWindow()
{
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

render::SurfaceRequest GameWindow::Surface() const
{
    Uint32 count = 0;
    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (names == nullptr)
    {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions: ") +
                                 SDL_GetError());
    }
    render::SurfaceRequest request;
    request.instance_extensions.assign(names, names + count);
    SDL_Window *window = window_;
    request.create_surface = [window](VkInstance instance)
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
        {
            return VkSurfaceKHR{VK_NULL_HANDLE};
        }
        return surface;
    };
    return request;
}

VkExtent2D GameWindow::Drawable() const
{
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &width, &height))
    {
        throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels: ") + SDL_GetError());
    }
    return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

void GameWindow::CaptureMouse(bool captured) const
{
    if (!SDL_SetWindowRelativeMouseMode(window_, captured))
    {
        throw std::runtime_error(std::string("SDL_SetWindowRelativeMouseMode: ") + SDL_GetError());
    }
}

} // namespace gears::engine::app
