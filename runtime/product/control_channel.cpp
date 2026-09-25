#include "control_channel.h"

#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <lucent/log.h>

#include "input.h"
#include "memory_request.h"
#include "navigation_probe.h"
#include "pad_form.h"
#include "player_probe.h"
#include "portable_pixmap.h"

namespace gears::product
{
namespace
{

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 8 * 1024;
constexpr std::size_t kMaxConnections = 8;

[[nodiscard]] lucent::http::ServerOptions LoopbackOptions(std::uint16_t port)
{
    lucent::http::ServerOptions options;
    options.port = port;
    options.listen_scope = lucent::http::ListenScope::Loopback;
    options.max_header_bytes = kMaxHeaderBytes;
    options.max_body_bytes = kMaxBodyBytes;
    options.max_connections = kMaxConnections;
    return options;
}

[[nodiscard]] std::string JsonString(std::string_view text)
{
    std::string quoted = "\"";
    for (char c : text)
    {
        if (c == '"' || c == '\\')
        {
            quoted.push_back('\\');
            quoted.push_back(c);
        }
        else if (c == '\n' || c == '\r')
        {
            quoted.push_back(' ');
        }
        else
        {
            quoted.push_back(c);
        }
    }
    quoted.push_back('"');
    return quoted;
}

[[nodiscard]] lucent::http::Response JsonError(int status, std::string reason,
                                               std::string_view message)
{
    return lucent::http::Response::json(status, std::move(reason),
                                        std::format("{{\"error\":{}}}\n", JsonString(message)));
}

// The pad fields of a request, from its body and its query alike.
[[nodiscard]] std::string PadFields(const lucent::http::Request &request)
{
    std::string fields = request.body;
    if (!request.query().empty())
    {
        if (!fields.empty())
        {
            fields.push_back('&');
        }
        fields += request.query();
    }
    return fields;
}

// A path's kind as the navigation route names it; an unknown class by vtable.
[[nodiscard]] std::string PathKindName(const titles::gears1::NavigationPath &path)
{
    switch (path.kind)
    {
    case titles::gears1::PathKind::Walk:
        return "walk";
    case titles::gears1::PathKind::Mantle:
        return "mantle";
    case titles::gears1::PathKind::Other:
        break;
    }
    return std::format("0x{:08X}", path.vtable);
}

// A point's kind as the navigation route names it; an unknown class by vtable.
[[nodiscard]] std::string PointKindName(const titles::gears1::NavigationPoint &point)
{
    switch (point.kind)
    {
    case titles::gears1::PointKind::PathNode:
        return "path";
    case titles::gears1::PointKind::Cover:
        return "cover";
    case titles::gears1::PointKind::Other:
        break;
    }
    return std::format("0x{:08X}", point.vtable);
}

constexpr double kMedian = 0.5;
constexpr double kP95 = 0.95;
constexpr double kP99 = 0.99;
constexpr double kMicrosecondsPerMillisecond = 1000.0;

// A frame-time percentile in milliseconds, as the upper edge of its 0.1 ms
// bucket; `at_least` when it fell in the open-ended last bucket; null when
// the interval recorded no frame.
[[nodiscard]] std::string QuantileJson(const x360port::FrameIntervalHistogram &intervals,
                                       double fraction)
{
    std::optional<x360port::FrameIntervalQuantile> quantile = intervals.Quantile(fraction);
    if (!quantile)
    {
        return "null";
    }
    return std::format("{{\"ms\":{:.1f},\"at_least\":{}}}",
                       quantile->microseconds / kMicrosecondsPerMillisecond,
                       quantile->open_ended ? "true" : "false");
}

} // namespace

ControlChannel::ControlChannel(const x360port::SystemSession &session, RunStop *stop,
                               std::uint16_t port)
    : session_(session), stop_(stop),
      perf_mark_{
          .time = std::chrono::steady_clock::now(), .presents = 0, .intervals = {}, .counts = {}},
      server_(LoopbackOptions(port),
              [this](const lucent::http::Request &request) { return Handle(request); })
{
}

ControlChannel::~ControlChannel()
{
    server_.stop();
}

bool ControlChannel::Start()
{
    if (!server_.start())
    {
        return false;
    }
    lucent::info("control", "control channel serving on loopback port {}", server_.port());
    return true;
}

lucent::http::Response ControlChannel::Handle(const lucent::http::Request &request) const
{
    std::string_view path = request.path();
    if (request.method == "GET" && path == "/api/status")
    {
        return Status();
    }
    if (request.method == "POST" && path == "/api/input")
    {
        return SetPad(request);
    }
    if (request.method == "POST" && path == "/api/input/release")
    {
        ReleaseRemotePad();
        return Status();
    }
    if (request.method == "DELETE" && path == "/api/input")
    {
        DisconnectRemotePad();
        return Status();
    }
    if (request.method == "GET" && path == "/api/frame.ppm")
    {
        return Frame();
    }
    if (request.method == "GET" && path == "/api/memory")
    {
        return Memory(request);
    }
    if (request.method == "GET" && path == "/api/player")
    {
        return Player();
    }
    if (request.method == "GET" && path == "/api/navigation")
    {
        return Navigation();
    }
    if (request.method == "GET" && path == "/api/perf")
    {
        return Perf();
    }
    if (request.method == "POST" && path == "/api/stop")
    {
        return Stop();
    }
    return JsonError(404, "Not Found", std::format("no route {} {}", request.method, path));
}

lucent::http::Response ControlChannel::Status() const
{
    PadSnapshot pad = ReadPadSnapshot();
    return lucent::http::Response::json(
        200, "OK",
        std::format("{{\"presents\":{},\"input\":{{\"source\":{},\"connected\":{},\"packet\":{},"
                    "\"buttons\":{},\"lt\":{},\"rt\":{},\"lx\":{},\"ly\":{},\"rx\":{},"
                    "\"ry\":{}}}}}\n",
                    session_.PresentedFrameCount(),
                    JsonString(InputSourceName(CurrentInputSource())),
                    pad.connected ? "true" : "false", pad.packet, pad.state.buttons,
                    pad.state.leftTrigger, pad.state.rightTrigger, pad.state.thumbLX,
                    pad.state.thumbLY, pad.state.thumbRX, pad.state.thumbRY));
}

lucent::http::Response ControlChannel::Stop() const
{
    if (stop_ == nullptr)
    {
        return JsonError(409, "Conflict", "the windowed product ends when its window closes");
    }
    stop_->Request();
    lucent::info("control", "a stop was requested; the run ends at its next second");
    return lucent::http::Response::json(202, "Accepted", "{\"stopping\":true}\n");
}

lucent::http::Response ControlChannel::Perf() const
{
    PerfMark now{.time = std::chrono::steady_clock::now(),
                 .presents = session_.PresentedFrameCount(),
                 .intervals = session_.FrameIntervals(),
                 .counts = session_.ExecutionCounts()};
    PerfMark previous;
    {
        std::scoped_lock lock(perf_mutex_);
        previous = std::exchange(perf_mark_, now);
    }
    double seconds = std::chrono::duration<double>(now.time - previous.time).count();
    std::uint64_t presents = now.presents - previous.presents;
    x360port::FrameIntervalHistogram intervals = now.intervals.Since(previous.intervals);
    return lucent::http::Response::json(
        200, "OK",
        std::format("{{\"seconds\":{:.3f},\"presents\":{},\"presents_per_second\":{:.1f},"
                    "\"frame_ms\":{{\"count\":{},\"p50\":{},\"p95\":{},\"p99\":{},"
                    "\"max\":{}}},\"translated_functions\":{},\"new_translations\":{},"
                    "\"translation_failures\":{},\"native_override_calls\":{}}}\n",
                    seconds, presents,
                    seconds > 0.0 ? static_cast<double>(presents) / seconds : 0.0,
                    intervals.Count(), QuantileJson(intervals, kMedian),
                    QuantileJson(intervals, kP95), QuantileJson(intervals, kP99),
                    QuantileJson(intervals, 1.0), now.counts.translated_functions,
                    now.counts.translated_functions - previous.counts.translated_functions,
                    now.counts.translation_failures,
                    now.counts.native_override_calls - previous.counts.native_override_calls));
}

lucent::http::Response ControlChannel::SetPad(const lucent::http::Request &request)
{
    PadState pad;
    std::string error;
    if (!ParsePadForm(PadFields(request), pad, error))
    {
        return JsonError(400, "Bad Request", error);
    }
    if (!SetRemotePad(pad))
    {
        return JsonError(409, "Conflict", "a scripted walk owns the pad for this run");
    }
    return lucent::http::Response::json(200, "OK", "{}\n");
}

lucent::http::Response ControlChannel::Frame() const
{
    x360port::SystemFrameImage image;
    if (x360port::RuntimeFailure failure = session_.CaptureGuestOutput(image))
    {
        return JsonError(503, "Service Unavailable", failure.detail);
    }
    return lucent::http::Response::binary(200, "OK", "image/x-portable-pixmap",
                                          EncodePortablePixmap(image));
}

lucent::http::Response ControlChannel::Memory(const lucent::http::Request &request) const
{
    MemoryRequest read;
    std::string error;
    if (!ParseMemoryRequest(request.query(), read, error))
    {
        return JsonError(400, "Bad Request", error);
    }
    std::string bytes(read.length, '\0');
    if (x360port::RuntimeFailure failure =
            session_.ReadGuestMemory(read.address, std::as_writable_bytes(std::span(bytes))))
    {
        return JsonError(422, "Unprocessable Content", failure.detail);
    }
    return lucent::http::Response::binary(200, "OK", "application/octet-stream", std::move(bytes));
}

titles::gears1::GuestMemoryReader ControlChannel::GuestReader() const
{
    return [this](std::uint32_t address, std::span<std::byte> bytes)
    { return session_.ReadGuestMemory(address, bytes); };
}

lucent::http::Response ControlChannel::Player() const
{
    titles::gears1::PlayerSnapshot player;
    std::string error;
    if (!titles::gears1::ReadPlayer(GuestReader(), player, error))
    {
        return JsonError(409, "Conflict", error);
    }
    std::string pawn = "null";
    if (player.has_pawn)
    {
        std::string weapon = "null";
        if (player.has_weapon)
        {
            weapon = std::format("{{\"id\":{},\"rounds_fired\":{}}}", player.weapon,
                                 player.rounds_fired);
        }
        pawn = std::format("{{\"location\":[{},{},{}],\"health\":{},\"team\":{},\"weapon\":{}}}",
                           player.location[0], player.location[1], player.location[2],
                           player.health, player.team, weapon);
    }
    std::string pawns;
    for (const titles::gears1::PawnReading &listed : player.pawns)
    {
        pawns += std::format("{}{{\"id\":{},\"location\":[{},{},{}],\"health\":{},\"team\":{},"
                             "\"is_player\":{}}}",
                             pawns.empty() ? "" : ",", listed.address, listed.location[0],
                             listed.location[1], listed.location[2], listed.health, listed.team,
                             listed.is_player);
    }
    return lucent::http::Response::json(
        200, "OK",
        std::format("{{\"control_yaw\":{},\"control_pitch\":{},\"camera_yaw\":{},"
                    "\"camera_location\":[{},{},{}],\"world_seconds\":{},\"pawn\":{},"
                    "\"pawns\":[{}]}}\n",
                    player.control_yaw, player.control_pitch, player.camera_yaw,
                    player.camera_location[0], player.camera_location[1], player.camera_location[2],
                    player.world_seconds, pawn, pawns));
}

lucent::http::Response ControlChannel::Navigation() const
{
    std::vector<titles::gears1::NavigationPoint> points;
    std::string error;
    if (!titles::gears1::ReadNavigation(GuestReader(), points, error))
    {
        return JsonError(409, "Conflict", error);
    }
    std::string body = "{\"points\":[";
    for (std::size_t index = 0; index < points.size(); ++index)
    {
        const titles::gears1::NavigationPoint &point = points[index];
        body += std::format("{}{{\"id\":{},\"kind\":{},\"location\":[{},{},{}],\"yaw\":{},"
                            "\"paths\":[",
                            index == 0 ? "" : ",", point.address, JsonString(PointKindName(point)),
                            point.location[0], point.location[1], point.location[2], point.yaw);
        for (std::size_t path = 0; path < point.paths.size(); ++path)
        {
            const titles::gears1::NavigationPath &reach = point.paths[path];
            body += std::format("{}[{},{},{}]", path == 0 ? "" : ",", reach.end, reach.distance,
                                JsonString(PathKindName(reach)));
        }
        body += "]}";
    }
    body += "]}\n";
    return lucent::http::Response::json(200, "OK", std::move(body));
}

} // namespace gears::product
