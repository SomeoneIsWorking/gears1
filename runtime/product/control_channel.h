#pragma once

#include <cstdint>

#include <lucent/http.h>
#include <x360port/system_session.hpp>

namespace gears::product
{

// The loopback routes through which a maintainer drives an offscreen run
// while it plays: the title's pad, and the guest output it last presented.
//
//   GET    /api/status     presents, the pad's source and state
//   POST   /api/input      buttons=A,START&lx=..&ly=..&rx=..&ry=..&lt=..&rt=..
//   POST   /api/input/release
//   DELETE /api/input
//   GET    /api/frame.ppm  the latest guest output
//
// The pad is refused (409) while a scripted walk owns it.
class ControlChannel final
{
  public:
    ControlChannel(const x360port::SystemSession &session, std::uint16_t port);
    ControlChannel(const ControlChannel &) = delete;
    ControlChannel &operator=(const ControlChannel &) = delete;
    ~ControlChannel();

    // Starts serving; false if the port cannot be bound.
    [[nodiscard]] bool Start();

  private:
    [[nodiscard]] lucent::http::Response Handle(const lucent::http::Request &request) const;
    [[nodiscard]] lucent::http::Response Status() const;
    [[nodiscard]] static lucent::http::Response SetPad(const lucent::http::Request &request);
    [[nodiscard]] lucent::http::Response Frame() const;

    const x360port::SystemSession &session_;
    lucent::http::Server server_;
};

} // namespace gears::product
