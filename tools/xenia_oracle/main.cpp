// A HEADLESS Xenia harness: run the title, write frames as PNGs, no window.
//
// WHY THIS EXISTS RATHER THAN DRIVING xenia_canary. That binary is a windowed
// app: it creates a GTK window whatever `--headless` says, so every run puts a
// window on the operator's screen and every measurement depends on a desktop
// session being there. Xenia's own console tools (xenia-gpu-vulkan-trace-dump)
// prove the emulator core needs none of that -- they build an Emulator with a
// null display window and never touch the UI. This is the same construction,
// pointed at a TITLE instead of a trace.
//
// The one thing that had to be fixed in the fork to make it possible is
// `offscreen_presentation`: Emulator::Setup gated the presenter on having a
// display window, so a windowless emulator had nowhere to present and no guest
// output image to capture. graphics_system.cc always contemplated the case
// ("May be needed for offscreen use, such as capturing the guest output image")
// -- it was simply unreachable.
//
// WHAT IT IS FOR. The oracle question is "what should this frame look like?".
// Answering it from our own captures (tools/gfr_to_xtr.py) can only ever be as
// good as our reading of the guest's command stream. Answering it from the
// title running under Xenia does not depend on our reading at all, which is the
// whole point of a reference implementation.
//
//   tools/xenia_oracle --target=<iso> --oracle_out=<dir>
//                      [--oracle_seconds=N] [--oracle_interval=N]
//                      [--oracle_input=A@30,A@35,...]
//
// Frames are written as <dir>/frame_<seconds>s.png on a fixed wall-clock
// cadence, so a run that renders nothing leaves an empty directory and a log
// line saying so, rather than one ambiguous file.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "xenia/gpu/command_processor.h"
#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/apu/sdl/sdl_audio_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/hid/input_driver.h"
#include "xenia/ui/presenter.h"

#include "scripted_input.h"

#include "third_party/stb/stb_image_write.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "third_party/stb/stb_image_write.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

// The positional argument named in XE_DEFINE_CONSOLE_APP below must be a cvar
// that EXISTS. Naming one that does not makes cxxopts throw, and the failure
// then went to a zenity dialog rather than to stderr (fixed in the fork, but
// the cvar still has to be declared).
DEFINE_transient_path(target, "", "The .iso or .xex to run.", "Oracle");

DEFINE_path(oracle_out, "scratch/oracle/frames",
            "Directory to write captured frames into.", "Oracle");
DEFINE_int32(oracle_seconds, 120, "How long to let the title run, in seconds.",
             "Oracle");
DEFINE_int32(oracle_interval, 10,
             "Seconds between captured frames. One capture per interval.",
             "Oracle");
// A headless run has no operator, and this title does not leave its title
// screen or its storage-device dialog on its own -- it waits, and every frame
// captured is black for as long as it waits. The default is what a person does
// to get in: START once the title screen is up, then A over and over.
// Ask Xenia to capture ONE of its own frames as a .xtr at this many seconds
// in, 0 to never. Written under --trace_gpu_prefix.
//
// This is the control arm for our synthesised traces (tools/gfr_to_xtr.py): a
// trace XENIA produced, dumped by the same xenia-gpu-vulkan-trace-dump that
// renders ours black. If Xenia's own trace renders, the dump path is sound and
// the fault is in what we synthesise; if it is also black, no comparison built
// on that tool means anything. Neither answer is available by inspection.
DEFINE_int32(oracle_trace_at, 0,
             "Seconds in at which to capture a Xenia GPU trace of one frame.",
             "Oracle");

// FRAME-DRIVEN MODE. With this set, --oracle_input's numbers are GUEST FRAMES
// rather than seconds, captures happen at frame indices rather than wall-clock
// offsets, and the run ends after --oracle_frames frames instead of
// --oracle_seconds. That is what lets a comparison against our renderer mean
// something per pixel: both sides are driven and sampled by the guest's own
// frame counter, so frame N is the same game moment on both for as long as the
// title is deterministic under identical input.
DEFINE_bool(oracle_by_frame, false,
            "Drive input and captures by GUEST FRAME instead of wall clock.",
            "Oracle");
DEFINE_int32(oracle_frames, 3000,
             "With --oracle_by_frame: guest frames to run before stopping.",
             "Oracle");
DEFINE_int32(oracle_frame_interval, 300,
             "With --oracle_by_frame: guest frames between captures.",
             "Oracle");
DEFINE_int32(oracle_frame_timeout, 240,
             "With --oracle_by_frame: seconds to wait for the counter to reach "
             "the next target before giving up. A title that hangs has to end "
             "the run, not wedge it forever.",
             "Oracle");

DEFINE_string(oracle_input, "START@25+8,A@30+2",
              "Scripted controller presses, BUTTON@SECONDS[+REPEAT_SECONDS], "
              "comma separated. Empty disables input entirely.",
              "Oracle");

namespace xe {
namespace oracle {

// The guest output image, as the presenter last refreshed it. Returns false
// when there is nothing to capture -- which is a real answer (the title has not
// presented yet), not an error, and the caller says so per frame rather than
// writing a black PNG that looks like a rendering result.
bool WriteFramePng(gpu::GraphicsSystem* graphics_system,
                   const std::filesystem::path& path) {
  ui::Presenter* presenter = graphics_system->presenter();
  if (!presenter) {
    XELOGE("oracle: no presenter -- the emulator was built without offscreen "
           "presentation, so no frame can ever be captured");
    return false;
  }
  ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image)) {
    return false;
  }
  FILE* handle = filesystem::OpenFile(path, "wb");
  if (!handle) {
    XELOGE("oracle: cannot open {} for writing", xe::path_to_utf8(path));
    return false;
  }
  auto write = [](void* context, void* data, int size) {
    fwrite(data, 1, size, reinterpret_cast<FILE*>(context));
  };
  stbi_write_png_to_func(write, handle, static_cast<int>(image.width),
                         static_cast<int>(image.height), 4, image.data.data(),
                         static_cast<int>(image.stride));
  fclose(handle);
  XELOGI("oracle: wrote {} ({}x{})", xe::path_to_utf8(path), image.width,
         image.height);
  return true;
}

int oracle_main(const std::vector<std::string>& args) {
  std::filesystem::path target = cvars::target;
  if (target.empty() && args.size() >= 2) {
    target = xe::to_path(args[1]);
  }
  if (target.empty()) {
    XELOGE("oracle: no target given. Pass the .iso as the first argument.");
    return 5;
  }
  if (!std::filesystem::exists(target)) {
    // Refused rather than attempted: a missing image would otherwise surface
    // much later as "the title never launched", which is a different problem.
    XELOGE("oracle: {} does not exist. Nothing was run.",
           xe::path_to_utf8(target));
    return 5;
  }

  std::vector<gears::ScriptedPress> presses;
  gears::ScriptedInputDriver* scripted_driver = nullptr;
  if (!cvars::oracle_input.empty()) {
    std::string error;
    if (!gears::ParseInputScript(cvars::oracle_input, presses, error)) {
      // Refused rather than run without input: a run with a schedule that
      // failed to parse would sit on the title screen and produce black frames
      // that look like a rendering problem.
      XELOGE("oracle: --oracle_input is not usable: {}. Nothing was run.",
             error);
      return 5;
    }
    XELOGI("oracle: {} scheduled press(es) from \"{}\"", presses.size(),
           cvars::oracle_input);
  } else {
    XELOGW("oracle: input is DISABLED, so the title will sit wherever it first "
           "waits for a button. Expect black frames.");
  }

  auto emulator = std::make_unique<Emulator>("", "", "", "");
  // Null window, null ImGui drawer -- and offscreen presentation ON, which is
  // the whole reason a windowless run can produce an image at all.
  X_STATUS result = emulator->Setup(
      nullptr, nullptr, false, true,
      // An audio system is NOT optional for a running title, and the NOP one is
      // not enough either. Both were measured:
      //   nullptr      -- the guest boots as far as
      //                   XAudioRegisterRenderDriverClient and stops; with no
      //                   APU its render thread never starts and no frame is
      //                   ever drawn.
      //   nop          -- the guest gets further, draws ~124 frames, then
      //                   CRASHES inside its own render-driver callback:
      //                   PC 0x825F39F4, read of 0x10000003C, where 0x825F3450
      //                   is the callback it registered. Silent until the crash
      //                   dump was moved ahead of Emulator::Pause().
      // So a real backend it is. SDL with SDL_AUDIODRIVER=dummy needs no sound
      // device, which keeps this usable on a machine with no audio at all.
      apu::sdl::SDLAudioSystem::Create,
      []() -> std::unique_ptr<gpu::GraphicsSystem> {
        return std::make_unique<gpu::vulkan::VulkanGraphicsSystem>();
      },
      [&presses, &scripted_driver](ui::Window* window)
          -> std::vector<std::unique_ptr<hid::InputDriver>> {
        std::vector<std::unique_ptr<hid::InputDriver>> drivers;
        if (!presses.empty()) {
          auto driver =
              std::make_unique<gears::ScriptedInputDriver>(window, 0, presses);
          // Kept so the frame tick source can be attached once the graphics
          // system exists. The emulator owns the driver, so this is a borrowed
          // pointer and must not outlive it -- it is only used below, while the
          // emulator is alive.
          scripted_driver = driver.get();
          drivers.push_back(std::move(driver));
        }
        return drivers;
      });
  if (XFAILED(result)) {
    XELOGE("oracle: failed to set up the emulator: {:08X}", result);
    return 4;
  }

  result = emulator->LaunchPath(target);
  if (XFAILED(result)) {
    XELOGE("oracle: failed to launch {}: {:08X}", xe::path_to_utf8(target),
           result);
    return 3;
  }
  XELOGI("oracle: launched {}", xe::path_to_utf8(target));

  const std::filesystem::path out_dir = cvars::oracle_out;
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  // FRAME-DRIVEN: hand the input driver the guest's own frame counter, so the
  // schedule advances with the game rather than with the wall clock.
  if (cvars::oracle_by_frame) {
    if (!scripted_driver) {
      XELOGE("oracle: --oracle_by_frame needs a scripted input schedule, and "
             "--oracle_input is empty. Nothing would drive the title, so this "
             "would produce a filmstrip of the title screen indexed by frame. "
             "Refusing.");
      return 5;
    }
    gpu::GraphicsSystem* gs = emulator->graphics_system();
    scripted_driver->SetFrameTickSource([gs]() -> uint64_t {
      gpu::CommandProcessor* cp = gs ? gs->command_processor() : nullptr;
      return cp ? cp->guest_swap_count() : 0;
    });
    XELOGI("oracle: input and captures are driven by the GUEST FRAME COUNTER; "
           "--oracle_input numbers are frames, not seconds");
  }

  if (cvars::oracle_by_frame) {
    const int32_t total = std::max(1, cvars::oracle_frames);
    const int32_t step = std::max(1, cvars::oracle_frame_interval);
    gpu::GraphicsSystem* gs = emulator->graphics_system();
    int captured_f = 0, attempted_f = 0;
    bool trace_requested_f = false;
    uint64_t last_seen = 0;
    for (int32_t target = step; target <= total; target += step) {
      // Wait for the guest to REACH this frame. A title that stops presenting
      // must end the run rather than wedge it, and the timeout says which
      // frame it died at instead of leaving an empty directory to interpret.
      const auto deadline =
          std::chrono::steady_clock::now() +
          std::chrono::seconds(std::max(1, cvars::oracle_frame_timeout));
      for (;;) {
        gpu::CommandProcessor* cp = gs ? gs->command_processor() : nullptr;
        last_seen = cp ? cp->guest_swap_count() : 0;
        if (last_seen >= static_cast<uint64_t>(target)) {
          break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          XELOGE("oracle: STOPPED at guest frame {} waiting for frame {} -- "
                 "the title stopped presenting. {} of {} captures were taken; "
                 "the filmstrip is SHORT, not complete.",
                 last_seen, target, captured_f, attempted_f);
          target = total + 1;  // end the outer loop
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (target > total) {
        break;
      }
      ++attempted_f;
      if (cvars::oracle_trace_at > 0 && !trace_requested_f &&
          target >= cvars::oracle_trace_at) {
        trace_requested_f = true;
        XELOGI("oracle: requesting a Xenia frame trace at guest frame {}",
               target);
        emulator->graphics_system()->RequestFrameTrace();
      }
      // Named by the frame index the capture was TAKEN AT, which is the key the
      // comparison joins on. The counter may have advanced past the target
      // while the readback ran; the name records the target, and the log the
      // actual, so a skew is visible rather than assumed away.
      if (WriteFramePng(emulator->graphics_system(),
                        out_dir / fmt::format("frame_{:06d}.png", target))) {
        ++captured_f;
        XELOGI("oracle: captured guest frame {} (counter was {})", target,
               last_seen);
      } else {
        XELOGI("oracle: nothing to capture at guest frame {} -- the title has "
               "not presented yet",
               target);
      }
    }
    XELOGI("oracle: captured {} of {} attempts over {} guest frames into {}",
           captured_f, attempted_f, total, xe::path_to_utf8(out_dir));
    emulator.reset();
    return captured_f > 0 ? 0 : 6;
  }

  const int32_t seconds = std::max(1, cvars::oracle_seconds);
  const int32_t interval = std::max(1, cvars::oracle_interval);
  int captured = 0, attempted = 0;
  bool trace_requested = false;
  for (int32_t elapsed = interval; elapsed <= seconds; elapsed += interval) {
    std::this_thread::sleep_for(std::chrono::seconds(interval));
    ++attempted;
    if (cvars::oracle_trace_at > 0 && !trace_requested &&
        elapsed >= cvars::oracle_trace_at) {
      trace_requested = true;
      // The request is served on the GPU worker thread, and only at a SWAP: the
      // trace opens at one swap and closes at the next. So it does nothing
      // until the title presents, and a request made while the title is
      // loading (2 draws a frame) produces no file at all. The wait after the
      // loop is what makes that visible instead of silent.
      XELOGI("oracle: requesting a Xenia frame trace at {}s", elapsed);
      emulator->graphics_system()->RequestFrameTrace();
    }
    if (WriteFramePng(emulator->graphics_system(),
                      out_dir / fmt::format("frame_{:04d}s.png", elapsed))) {
      ++captured;
    } else {
      // Said out loud every time. Silence here would be indistinguishable from
      // a run that was never scheduled.
      XELOGI("oracle: nothing to capture at {}s -- the title has not presented "
             "a frame yet",
             elapsed);
    }
  }

  XELOGI("oracle: captured {} of {} attempts over {}s into {}", captured,
         attempted, seconds, xe::path_to_utf8(out_dir));

  // WAIT FOR THE FRAME TRACE BEFORE TEARING ANYTHING DOWN.
  //
  // A requested trace is written from the GPU worker thread between one swap
  // and the next. Destroying the emulator inside that window closes the trace
  // writer's FILE* under a thread that is inside fwrite: glibc aborts with
  // "free(): invalid pointer" and the .xtr on disk is TRUNCATED -- 4.4 MB of a
  // 42 MB trace, which loads, plays back a fraction of a frame and looks like a
  // trace of a frame that did almost nothing.
  //
  // Bounded, and it says which way it ended. A trace that never completes is a
  // real outcome here (the request lands only at a swap, so a title that is
  // loading never serves it), and it must not be reported as success.
  if (trace_requested) {
    constexpr int kTraceWaitSeconds = 60;
    int waited = 0;
    while (emulator->graphics_system()->IsFrameTracePending() &&
           waited < kTraceWaitSeconds) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      ++waited;
    }
    if (emulator->graphics_system()->IsFrameTracePending()) {
      XELOGE(
          "oracle: the frame trace was still being written after {}s, so the "
          "run is shutting down ON TOP of it -- the .xtr under "
          "--trace_gpu_prefix is TRUNCATED and must not be used. The request "
          "is served at a SWAP, so a title "
          "that is not presenting never serves it",
          kTraceWaitSeconds);
    } else {
      XELOGI("oracle: the frame trace finished writing after {}s", waited);
    }
  }

  emulator.reset();
  // A run that captured nothing is a FAILED run, not an empty one: exiting 0
  // would let a harness above this treat "no frames" as success.
  return captured ? 0 : 1;
}

}  // namespace oracle
}  // namespace xe

XE_DEFINE_CONSOLE_APP("xenia_oracle", xe::oracle::oracle_main, "game.iso",
                      "target");
