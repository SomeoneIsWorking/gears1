#include "debug_http.h"

#include "graphics_probe.h"
#include "host_product_identity.h"
#include "input.h"
#include "render_thread.h"

#include <lucent/config.h>
#include <lucent/http.h>
#include <lucent/log.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gears
{
namespace
{

constexpr std::chrono::seconds kProbeTimeout{10};
std::unique_ptr<lucent::http::Server> g_server;

std::string JsonError(std::string_view message)
{
    std::string escaped;
    escaped.reserve(message.size());
    for (const char c : message)
    {
        if (c == '\\' || c == '"')
            escaped.push_back('\\');
        if (c == '\n' || c == '\r')
            escaped.push_back(' ');
        else
            escaped.push_back(c);
    }
    return std::format("{{\"error\":\"{}\"}}\n", escaped);
}

template <typename Integer>
bool ParseBoundedInteger(std::string_view text, Integer minimum, Integer maximum, Integer &out)
{
    int64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed < int64_t(minimum) || parsed > int64_t(maximum))
        return false;
    out = Integer(parsed);
    return true;
}

bool ParseButtons(std::string_view text, uint16_t &buttons, std::string &error)
{
    buttons = 0;
    while (!text.empty())
    {
        const size_t separator = text.find(',');
        const std::string_view name = text.substr(0, separator);
        uint16_t button = 0;
        if (name.empty() || !PadButtonByName(name, button))
        {
            error = std::format("unknown button name '{}'", name);
            return false;
        }
        buttons |= button;
        if (separator == std::string_view::npos)
            break;
        text.remove_prefix(separator + 1);
    }
    return true;
}

bool ParsePad(const lucent::http::Request &request, PadState &pad, std::string &error)
{
    std::string encoded = request.body;
    if (!request.query().empty())
    {
        if (!encoded.empty())
            encoded.push_back('&');
        encoded += request.query();
    }

    std::vector<lucent::http::FormField> values;
    if (!lucent::http::parse_form_urlencoded(encoded, values, error))
        return false;
    for (const lucent::http::FormField &value : values)
    {
        bool valid = true;
        if (value.name == "buttons")
            valid = value.value.empty() || ParseButtons(value.value, pad.buttons, error);
        else if (value.name == "lx")
            valid = ParseBoundedInteger(value.value, int16_t(-32767), int16_t(32767), pad.thumbLX);
        else if (value.name == "ly")
            valid = ParseBoundedInteger(value.value, int16_t(-32767), int16_t(32767), pad.thumbLY);
        else if (value.name == "rx")
            valid = ParseBoundedInteger(value.value, int16_t(-32767), int16_t(32767), pad.thumbRX);
        else if (value.name == "ry")
            valid = ParseBoundedInteger(value.value, int16_t(-32767), int16_t(32767), pad.thumbRY);
        else if (value.name == "lt")
            valid = ParseBoundedInteger(value.value, uint8_t(0), uint8_t(255), pad.leftTrigger);
        else if (value.name == "rt")
            valid = ParseBoundedInteger(value.value, uint8_t(0), uint8_t(255), pad.rightTrigger);
        else
        {
            error = std::format("unknown input field '{}'", value.name);
            return false;
        }
        if (!valid)
        {
            if (error.empty())
                error = std::format("invalid value '{}' for {}", value.value, value.name);
            return false;
        }
    }
    return true;
}

std::string ProbeJson(const std::shared_ptr<const GraphicsProbeFrame> &probe)
{
    if (!probe)
        return "null";
    return std::format("{{\"request\":{},\"guest_frame\":{},\"rendered\":{},"
                       "\"width\":{},\"height\":{},\"draws\":{},\"shader_pairs\":{},"
                       "\"non_black_pixels\":{},\"pixel_hash\":\"{:016x}\","
                       "\"mean_rgb\":[{:.3f},{:.3f},{:.3f}]}}",
                       probe->request, probe->guestFrame, probe->rendered ? "true" : "false",
                       probe->width, probe->height, probe->draws, probe->shaderPairs,
                       probe->nonBlackPixels, probe->pixelHash, probe->meanRed, probe->meanGreen,
                       probe->meanBlue);
}

std::string StatusJson()
{
    uint32_t packet = 0;
    const PadState pad = CurrentPad(packet);
    const InputSource source = CurrentInputSource();
    const RenderThreadStats renderer = RenderThreadCounters();
    return std::format("{{\"service\":\"{}-debug\",\"guest_frames_presented\":{},"
                       "\"input\":{{\"connected\":{},\"source\":\"{}\",\"packet\":{},"
                       "\"buttons\":{},\"lt\":{},\"rt\":{},\"lx\":{},\"ly\":{},"
                       "\"rx\":{},\"ry\":{}}},"
                       "\"renderer\":{{\"submitted\":{},\"dropped\":{},\"rendered\":{},"
                       "\"busy_ms\":{},\"cpu_ms\":{},\"runqueue_ms\":{},"
                       "\"gpu_timing_available\":{},\"gpu_samples\":{},\"gpu_ns\":{},"
                       "\"gpu_max_ns\":{},"
                       "\"gpu_failed_samples\":{}}},"
                       "\"probe\":{}}}\n",
                       kHostProductKey, CurrentGuestFrame(), PadConnected() ? "true" : "false",
                       InputSourceName(source), packet, pad.buttons, pad.leftTrigger,
                       pad.rightTrigger, pad.thumbLX, pad.thumbLY, pad.thumbRX, pad.thumbRY,
                       renderer.submitted, renderer.dropped, renderer.rendered, renderer.busyMillis,
                       renderer.cpuMillis, renderer.runqueueMillis,
                       renderer.gpuTimingAvailable ? "true" : "false", renderer.gpuSamples,
                       renderer.gpuNanoseconds, renderer.gpuMaximumNanoseconds,
                       renderer.gpuFailedSamples, ProbeJson(LatestGraphicsProbe()));
}

lucent::http::Response PpmResponse(const GraphicsProbeFrame &frame)
{
    const std::string header = std::format("P6\n{} {}\n255\n", frame.width, frame.height);
    const size_t rgbBytes = size_t(frame.width) * frame.height * 3;
    std::string body = header;
    body.resize(header.size() + rgbBytes);
    char *destination = body.data() + header.size();
    for (size_t pixel = 0; pixel < size_t(frame.width) * frame.height; ++pixel)
    {
        destination[pixel * 3] = char(frame.rgba[pixel * 4]);
        destination[pixel * 3 + 1] = char(frame.rgba[pixel * 4 + 1]);
        destination[pixel * 3 + 2] = char(frame.rgba[pixel * 4 + 2]);
    }
    return lucent::http::Response::binary(200, "OK", "image/x-portable-pixmap", std::move(body));
}

lucent::http::Response HandleRequest(const lucent::http::Request &request)
{
    const std::string_view path = request.path();
    if (request.method == "GET" && path == "/")
    {
        return lucent::http::Response::text(
            200, "OK",
            std::format(
                "{} interactive debug API\n\n"
                "GET    /api/status\n"
                "POST   /api/input  buttons=A,START&lx=-32767&ly=32767&rx=0&ry=0&lt=0&rt=0\n"
                "POST   /api/input/release\n"
                "DELETE /api/input\n"
                "GET    /api/frame.ppm  (waits for the next renderer readback)\n",
                kHostProductName));
    }
    if (request.method == "GET" && path == "/api/status")
        return lucent::http::Response::json(200, "OK", StatusJson());
    if (request.method == "POST" && path == "/api/input")
    {
        PadState pad;
        std::string error;
        if (!ParsePad(request, pad, error))
            return lucent::http::Response::json(400, "Bad Request", JsonError(error));
        if (!SetRemotePad(pad))
            return lucent::http::Response::json(
                409, "Conflict", JsonError("GEARS_INPUT_SCRIPT owns the pad for this run"));
        return lucent::http::Response::json(200, "OK", StatusJson());
    }
    if (request.method == "POST" && path == "/api/input/release")
    {
        ReleaseRemotePad();
        return lucent::http::Response::json(200, "OK", StatusJson());
    }
    if (request.method == "DELETE" && path == "/api/input")
    {
        DisconnectRemotePad();
        return lucent::http::Response::json(200, "OK", StatusJson());
    }
    if (request.method == "GET" && path == "/api/frame.ppm")
    {
        const uint64_t requestId = RequestGraphicsProbe();
        const std::shared_ptr<const GraphicsProbeFrame> frame = WaitForGraphicsProbe(
            requestId, std::chrono::duration_cast<std::chrono::milliseconds>(kProbeTimeout));
        if (!frame)
            return lucent::http::Response::json(
                504, "Gateway Timeout", JsonError("no rendered frame arrived within 10 seconds"));
        if (!frame->rendered || frame->rgba.empty())
            return lucent::http::Response::json(
                503, "Service Unavailable",
                JsonError("the renderer could not produce an RGBA readback"));
        return PpmResponse(*frame);
    }
    return lucent::http::Response::json(404, "Not Found", JsonError("unknown endpoint"));
}

} // namespace

void StartDebugHttpServer()
{
    if (g_server)
        return;
    const long configured = lucent::config::number("DEBUG_HTTP_PORT", 32123);
    if (configured == 0)
    {
        lucent::info("http", "interactive debug API disabled by GEARS_DEBUG_HTTP_PORT=0");
        return;
    }
    if (configured < 1 || configured > std::numeric_limits<uint16_t>::max())
    {
        lucent::error("http",
                      "GEARS_DEBUG_HTTP_PORT={} is outside 1..65535;"
                      " interactive control is unavailable",
                      configured);
        return;
    }

    lucent::http::ServerOptions options;
    options.port = uint16_t(configured);
    options.max_header_bytes = 16 * 1024;
    options.max_body_bytes = 8 * 1024;
    options.max_connections = 8;
    auto server = std::make_unique<lucent::http::Server>(options, HandleRequest);
    if (!server->start())
    {
        lucent::error("http", "interactive debug API unavailable; set"
                              " GEARS_DEBUG_HTTP_PORT to another port for concurrent runs");
        return;
    }
    g_server = std::move(server);
    lucent::info("http", "Gears input and on-demand renderer probe routes are live");
}

void StopDebugHttpServer()
{
    if (!g_server)
        return;
    g_server->stop();
    g_server.reset();
}

} // namespace gears
