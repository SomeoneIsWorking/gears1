#include "control_channel.h"

#include <format>
#include <string>
#include <span>
#include <string_view>
#include <utility>

#include <lucent/log.h>

#include "input.h"
#include "memory_request.h"
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

} // namespace

ControlChannel::ControlChannel(const x360port::SystemSession &session, std::uint16_t port)
    : session_(session), server_(LoopbackOptions(port), [this](const lucent::http::Request &request)
                                 { return Handle(request); })
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

lucent::http::Response ControlChannel::Player() const
{
    titles::gears1::PlayerSnapshot player;
    std::string error;
    auto read = [this](std::uint32_t address, std::span<std::byte> bytes)
    { return session_.ReadGuestMemory(address, bytes); };
    if (!titles::gears1::ReadPlayer(read, player, error))
    {
        return JsonError(409, "Conflict", error);
    }
    std::string pawn = "null";
    if (player.has_pawn)
    {
        pawn = std::format("{{\"location\":[{},{},{}],\"magazine_rounds_fired\":{}}}",
                           player.location[0], player.location[1], player.location[2],
                           player.has_weapon ? std::to_string(player.magazine_rounds_fired)
                                             : std::string("null"));
    }
    return lucent::http::Response::json(
        200, "OK",
        std::format("{{\"control_yaw\":{},\"camera_yaw\":{},\"pawn\":{}}}\n", player.control_yaw,
                    player.camera_yaw, pawn));
}

} // namespace gears::product
