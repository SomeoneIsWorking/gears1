# gears1

An in-progress attempt at a PC-native port of **Gears of War** (Xbox 360) by
**static recompilation** — translating the game's PowerPC code to C++ ahead of
time and overriding only at hardware/OS seams (graphics, audio, input, file
I/O), following the [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) /
[N64Recomp](https://github.com/N64Recomp/N64Recomp) model.

**Status: early.** The executable recompiles, and guest code executes on
multiple threads, running past Xenos GPU initialisation on three threads. Nothing is
rendered yet. See
[`debug_journal/`](debug_journal/) for dated, honest write-ups of what has and
has not been verified.

## What works

- The XEX is decrypted and decompressed to a real PE image (13.5 MB, load base
  `0x82000000`).
- Register save/restore helpers and 360 jump tables are located automatically.
- XenonRecomp emits **49,012 functions** (~176 MB of C++) with **zero
  unimplemented instructions**, and all 193 translation units compile.
- The runtime maps the image, installs 49,475 functions into the indirect-call
  table, and runs guest code through memory allocation, the kernel object
  manager, timing and display queries — **74 of 226** kernel imports.
- **Guest threading works.** `ExCreateThread` gives each guest thread its own
  context, KPCR, TLS and stack; two guest threads run concurrently with the
  main thread, deterministically across runs.

## What does not

- **Nothing is rendered.** No graphics, audio, input or file I/O. 152 kernel
  imports are unimplemented; each aborts loudly with its name and arguments the
  first time the game calls it.
- **1,394 jump-table / function-boundary errors.** XenonRecomp's function
  analyser treats a jump table as a tail call and cuts functions short.
- **Boot dies during UE3 initialisation** — a float is dereferenced as a pointer.
  Twelve mechanisms eliminated; see `docs/issues/`.
- **No GPU backend.** The `Vd*` surface is a *null GPU*: it tracks driver state
  and retires command buffers without executing them, and the Xenos register
  file is inert memory. Registered graphics interrupts never fire. Nothing is
  rasterised or presented.

## You must supply the game

**No game data is included, and none ever will be.** This repository contains
only tooling and configuration. To use it you need your own legally obtained
Gears of War disc image.

```sh
export GEARS_ISO="/path/to/your/Gears of War.iso"     # or put it in .env
python3 tools/gdf_extract.py --extract default.xex --out scratch/bin/default.xex
```

Everything derived from the disc lands in `scratch/`, which is gitignored.

## Layout

| Path | |
|---|---|
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
      -DCMAKE_BUILD_TYPE=Release
cmake --build scratch/build-xenonrecomp

# jump tables, then the recompilation itself
./scratch/build-xenonrecomp/XenonAnalyse/XenonAnalyse \
    scratch/bin/default.xex scratch/config/gears_switch_tables.toml
./scratch/build-xenonrecomp/XenonRecomp/XenonRecomp \
    config/gears.toml extern/XenonRecomp/XenonUtils/ppc_context.h
```

XenonRecomp needs CMake 3.20+ and Clang 18+. It exits non-zero if any
instruction lacks an implementation.

Then build and run the runtime against the generated code:

```sh
cmake -S . -B scratch/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build scratch/build
./scratch/build/runtime/gears1 scratch/bin/default.xex
```

Set `GEARS_LUCENT_DEBUG=heap,loader,kernel,thread,mem` for per-subsystem
tracing (`all` for everything).

## Fork changes

[`SomeoneIsWorking/XenonRecomp`, branch `gears`](https://github.com/SomeoneIsWorking/XenonRecomp/tree/gears)
adds 36 instruction implementations Gears needs — mostly halfword and
saturating VMX forms — and, more importantly, makes an unimplemented
instruction emit a trap and fail the build instead of emitting a bare comment
and letting the following code run against stale registers.

## Licence

Tooling here is MIT. XenonRecomp is MIT (see the submodule). Gears of War is
copyright Epic Games / Microsoft; this project ships none of it.
