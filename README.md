# GearsUE3

GearsUE3 is an in-progress clean-code engine port for the Xbox 360 Gears of War
games. It combines static recompilation with shared host Xbox services and
runtime native overrides. One exact title/revision is generated locally from a
user-owned disc and linked against the shared engine; game-derived generated
code and data never enter this repository.

Gears of War 1 is the first and currently only verified target. Gears 2, Gears
3, and Gears of War: Judgment are product scope, not present compatibility
claims. See `docs/gearsue3-engine.md` for the ownership and evidence model.

**Status: it boots, it plays its own menus, and it renders.** The title runs
from boot through its startup movies into the main menu, takes a controller into
the Act 1 campaign, and the guest-draw backend renders the game's own frames at
~30 fps. The in-game 3D world renders but is **not yet faithful** — see below.
`docs/re-frontier.md` tracks what is real reverse-engineering and what is still
approximate, `docs/codemap.md` says where each subsystem lives, and
[`debug_journal/`](debug_journal/) carries dated, honest write-ups.

## What works

- The XEX is decrypted and decompressed to a real PE image (13.5 MB, load base
  `0x82000000`), and XenonRecomp emits **~49,000 functions** (~176 MB of C++)
  with **zero unimplemented instructions**.
- **The title boots and plays.** It loads its own packages, plays the startup
  movies, walks its menus under a scripted or real controller, and reaches Act 1
  gameplay. ~102 of 226 kernel imports are implemented; the rest abort loudly
  with their name and arguments rather than stubbing silently.
- **Guest threading, heaps and timing are real.** Each guest thread gets its own
  context, KPCR, TLS and stack. Heap use plateaus and the title has run past
  frame 25,000 at a steady ~30 fps.
- **Input is the console's own.** `XamInput*` fills the real `X_INPUT_STATE`
  structures from an SDL gamepad, the keyboard, or a scripted timeline so a
  headless run is reproducible.
- **The renderer draws the guest's own frames.** The PM4 command processor
  executes the ring, the title's Xenos shaders are translated to SPIR-V through
  Xenia's translator, and every draw of a frame is issued with the guest's own
  geometry, textures, constants, viewport and output-merger state into
  per-EDRAM-surface render targets. Menus render correctly; the in-game HUD and
  world both render.
- **Audio works, end to end.** The render-driver pump asks the title for a
  frame at the console's 187.5 Hz, the title mixes one — decoding XMA through
  the hardware register block and context protocol to do it — and SDL plays the
  result. The decode is verified against an independent reference decode of the
  title's own streams at correlation 1.000000 over a full 142-second stream.

## What does not

- **The in-game world is not yet faithful.** A third of the world draws are
  still lost at clipping, the second predicated tile (the bottom 208 rows) is
  empty, and the HDR-to-LDR tonemap blows out. Each of these is measured and
  tracked on `docs/re-frontier.md` rather than guessed at.
- **Audio has unexercised paths.** Loop playback and true double-buffered
  streaming are ported from the reference but no stream has used them yet, and
  the pump falls behind its 187.5 Hz under a CPU-bound guest.
- **No networking, no user/content services.** Those imports abort on first call.
- **Saves do not complete yet.** The title now sees a storage device and creates
  its save content, which mounts a writable directory under the user's data
  path, but it asks for 2.6 GB on that path and is refused, and no save file
  has appeared on disk (catalog #45). Directory enumeration is still missing.

## You must supply the game

**No game or UE3 source is included, fetched, or accepted as a dependency.** To
build a playable title module you supply your own legally obtained Gears disc
image. Extraction and recompilation output stay under gitignored `scratch/`.

```sh
export GEARS_ISO="/path/to/your/Gears of War.iso"
python3 tools/gdf_extract.py "$GEARS_ISO" --extract-all scratch/game
```

Everything derived from the disc lands in `scratch/`, which is gitignored.

## Layout

| Path | |
|---|---|
| `run.sh` | Build and play (`--headless`, `--menu-walk`, `--no-build`, `--log`); `./run.sh --help` |
| `config/gears.toml` | XenonRecomp configuration — section addresses, register save/restore helpers |
| `tools/gdf_extract.py` | GDF/XDVDFS extractor for the Xbox 360 disc image |
| `tools/xex_probe/` | XEX decrypt/decompress, section + import dump, save/restore helper scan |
| `extern/XenonRecomp` | Submodule → our fork, `gears` branch |
| `docs/codemap.md` | Orientation map — what's where, and how far each subsystem really got |
| `docs/issues/` | Findings registry keyed by symptom (`tools/catalog.py search "..."`) |
| `debug_journal/` | Dated findings, including the dead ends |
| `scratch/` | All derived output (gitignored) |

## Build and run the recompiler

```sh
git clone --recursive https://github.com/SomeoneIsWorking/gears1
cmake -S extern/XenonRecomp -B scratch/build-xenonrecomp -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_COMPILER=clang
cmake --build scratch/build-xenonrecomp

# Recover jump tables, merge the tracked correction, and generate into a fresh
# ignored directory. Generated code is never patched after emission.
mkdir -p scratch/config
./scratch/build-xenonrecomp/XenonAnalyse/XenonAnalyse \
    scratch/game/default.xex scratch/config/gears_switch_tables.auto.toml
python3 tools/merge_switch_tables.py \
    scratch/config/gears_switch_tables.auto.toml \
    config/gears_switch_tables.extra.toml \
    scratch/config/gears_switch_tables.toml
tools/cleanup_scratch_path.sh scratch/ppc
mkdir -p scratch/ppc
./scratch/build-xenonrecomp/XenonRecomp/XenonRecomp \
    config/gears.toml extern/XenonRecomp/XenonUtils/ppc_context.h
```

XenonRecomp needs CMake 3.20+ and Clang 18+. It exits non-zero if any
instruction lacks an implementation.

Then build and run the runtime against the locally generated code:

```sh
./run.sh                 # configure if needed, build, and play
./run.sh --menu-walk     # ...driving itself from the title screen into Act 1
./run.sh --headless      # no window, for measurement
```

The default run also starts the loopback interactive debug API at
`http://127.0.0.1:32123`. It can drive the same controller state the guest reads
and request an authoritative renderer screenshot plus live counters after the
process has started. See [docs/interactive-debug.md](docs/interactive-debug.md).

`run.sh` is a thin wrapper over the three steps it saves you typing:

```sh
cmake -S . -B scratch/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build scratch/build
./scratch/build/runtime/gears1 scratch/game/default.xex scratch/game
```

`Debug` is the build type on purpose — a breakpoint in translated guest code has
to land somewhere meaningful — but it does **not** mean an unoptimised build. The
191 generated translation units are compiled at `GEARS_PPC_OPT` (default `-O2`)
and the host runtime at `GEARS_HOST_OPT` (default `-O2`), both with `-g`. Set
either to `-O0` to bisect a miscompile on that side. The host one was missing
until it was measured: the renderer's own frame cost fell from 45 ms to 29 ms the
moment it was compiled the way it had always claimed to be.

The second argument is the directory holding the title's data files, extracted
from the disc; `GEARS_GAME_DIR` sets the same thing. **It is not optional in
practice** — without it the runtime warns once and then every file open fails,
and the title runs just far enough to call `XamLoaderLaunchTitle` and quit,
which looks like a crash rather than a missing argument.

Set `GEARS_LUCENT_DEBUG=heap,loader,kernel,thread,mem` for per-subsystem
tracing (`all` for everything).

## Fork changes

[`SomeoneIsWorking/XenonRecomp`, branch `gears`](https://github.com/SomeoneIsWorking/XenonRecomp/tree/gears)
adds 36 instruction implementations Gears needs — mostly halfword and
saturating VMX forms — and, more importantly, makes an unimplemented
instruction emit a trap and fail the build instead of emitting a bare comment
and letting the following code run against stale registers.

## Third-party code

`extern/xenia` is a fork of [Xenia](https://github.com/xenia-canary/xenia-canary),
copyright (c) 2015 Ben Vanik and contributors, used under the BSD 3-Clause
licence — see `extern/xenia/LICENSE`, which is preserved verbatim.

Only its GPU translation subset is compiled here: the Xenos microcode front end,
the instruction-set definitions and the texture tiling code. Its GPU
abstraction, command processor and window system are not linked. Xenia has also
served as the reference for several hardware contracts this port had to get
right — the depth-sample-count record layout, and the packet predication rules
that the command processor now implements. Nothing in this repository is
endorsed by the Xenia project or its contributors.

## Licence

First-party tooling and engine code are intended to be MIT-licensed. XenonRecomp is MIT
(see the submodule). Xenia is
BSD 3-Clause (see above). The XMA decoder is built from a fork of FFmpeg
(`extern/ffmpeg-xmaframes`, LGPL 2.1 or later) that adds a per-frame XMA codec;
it is compiled from source by the build and linked statically. Gears of War is
copyright Epic Games / Microsoft; this project ships none of it. Building the
clean engine-owned tools does not require game material; generating and building
a playable title module requires the user's own disc.

See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES) for
the complete first- and third-party notices.
