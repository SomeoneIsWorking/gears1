#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

#include <lucent/http.h>
#include <x360port/system_session.hpp>

#include "guest_chain.h"
#include "run_stop.h"

namespace gears::product
{

// The loopback routes through which a maintainer drives and measures a run
// while it plays, offscreen or in the player's window: the title's pad, its
// performance, and the guest output it last presented.
//
//   GET    /api/status     presents, the pad's source and state
//   GET    /api/perf       presents per second, frame-time percentiles, and
//                          dynarec counts over the interval since the previous
//                          /api/perf (since launch on the first call)
//   POST   /api/input      buttons=A,START&lx=..&ly=..&rx=..&ry=..&lt=..&rt=..
//   POST   /api/input/release
//   DELETE /api/input
//   GET    /api/frame.ppm  the latest guest output
//   GET    /api/memory     address=0x..&length=N, guest memory as raw bytes
//   GET    /api/player     the local player's view, pawn, and the world's pawns; 409
//                          before gameplay
//   GET    /api/navigation the level's navigation points, each path as [end, distance,
//                          kind]; 409 before gameplay
//   POST   /api/stop       end an offscreen run at its next second, with its end-of-run
//                          checks; 409 in the windowed product, which ends when its
//                          window closes
//
// The pad is refused (409) while a scripted walk owns it.
class ControlChannel final
{
  public:
    // `stop` is the offscreen run's early end; null in the windowed product.
    ControlChannel(const x360port::SystemSession &session, RunStop *stop, std::uint16_t port);
    ControlChannel(const ControlChannel &) = delete;
    ControlChannel &operator=(const ControlChannel &) = delete;
    ~ControlChannel();

    // Starts serving; false if the port cannot be bound.
    [[nodiscard]] bool Start();

  private:
    [[nodiscard]] lucent::http::Response Handle(const lucent::http::Request &request) const;
    [[nodiscard]] lucent::http::Response Status() const;
    [[nodiscard]] lucent::http::Response Perf() const;
    [[nodiscard]] lucent::http::Response Stop() const;
    [[nodiscard]] static lucent::http::Response SetPad(const lucent::http::Request &request);
    [[nodiscard]] lucent::http::Response Frame() const;
    [[nodiscard]] lucent::http::Response Memory(const lucent::http::Request &request) const;
    [[nodiscard]] lucent::http::Response Player() const;
    [[nodiscard]] lucent::http::Response Navigation() const;
    [[nodiscard]] titles::gears1::GuestMemoryReader GuestReader() const;

    // The measurements /api/perf last reported from, so each call reports
    // the interval since the one before.
    struct PerfMark
    {
        std::chrono::steady_clock::time_point time;
        std::uint64_t presents = 0;
        x360port::FrameIntervalHistogram intervals;
        x360port::SystemExecutionCounts counts;
    };

    const x360port::SystemSession &session_;
    RunStop *stop_;
    mutable std::mutex perf_mutex_;
    mutable PerfMark perf_mark_;
    lucent::http::Server server_;
};

} // namespace gears::product
