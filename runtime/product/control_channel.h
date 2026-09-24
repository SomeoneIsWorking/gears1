#pragma once

#include <cstdint>

#include <lucent/http.h>
#include <x360port/system_session.hpp>

#include "guest_chain.h"
#include "run_stop.h"

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
//   GET    /api/memory     address=0x..&length=N, guest memory as raw bytes
//   GET    /api/player     the local player's view, pawn, and the world's pawns; 409
//                          before gameplay
//   GET    /api/navigation the level's navigation points, each path as [end, distance,
//                          kind]; 409 before gameplay
//   POST   /api/stop       end the run at its next second, with its end-of-run checks
//
// The pad is refused (409) while a scripted walk owns it.
class ControlChannel final
{
  public:
    ControlChannel(const x360port::SystemSession &session, RunStop &stop, std::uint16_t port);
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
    [[nodiscard]] lucent::http::Response Memory(const lucent::http::Request &request) const;
    [[nodiscard]] lucent::http::Response Player() const;
    [[nodiscard]] lucent::http::Response Navigation() const;
    [[nodiscard]] titles::gears1::GuestMemoryReader GuestReader() const;

    const x360port::SystemSession &session_;
    RunStop &stop_;
    lucent::http::Server server_;
};

} // namespace gears::product
