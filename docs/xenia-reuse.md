# Reusing Xenia's Xenos translator

The reconnaissance in `d3d-seam.md` put shader translation at the hard core of
the HLE backend: shaders reach the seam as Xenos microcode, and no amount of
knowing this is Unreal Engine changes that, because the microcode was produced
by the console's shader compiler and shipped in the cooked packages.

Xenia has already solved that problem, and it is BSD-3-Clause. This records what
is worth taking, how tightly it is bound to the rest of Xenia, and what the
obligations are.

## Licence obligations

BSD-3-Clause: permissive, but it requires the copyright notice, the condition
list and the disclaimer to be retained, and forbids using the project's or its
contributors' names to endorse this one. `gears1` is a public repository, so
this is a real obligation rather than a formality.

This is satisfied by taking a **fork** rather than copying a subset in, which
is both cleaner and less work to keep honest:

- `extern/xenia` is a submodule of `SomeoneIsWorking/xenia-canary`, a public
  fork of `xenia-canary/xenia-canary`, pinned at upstream commit `a635ac6`;
- upstream's `LICENSE`, copyright notices and full history come with it
  untouched, so retention is automatic rather than something to remember;
- the submodule boundary makes it obvious which code is ours and which is not,
  with no file-by-file provenance to maintain;
- changes we need (the capability shim below) are commits on the fork, so they
  are visibly ours and can be offered upstream or rebased.

Attribution still belongs in the top-level `README.md`, and the fork must stay
public or `git clone --recursive` breaks for anyone else.

## What the dependency graph actually looks like

Measured from the headers rather than assumed. The front end is shallow:

    shader_translator.h -> shader.h -> base/{byte_order,math,string_buffer},
                                       registers.h, ucode.h
    ucode.h             -> xenos.h  -> base/{math,memory}
    registers.h         -> base/assert, xenos.h, register_table.inc
    texture_info.h      -> xenos.h

So the microcode front end, the instruction-set definitions and the texture
tiling code come across with a handful of `xenia/base` headers behind them
(math, memory, byte_order, string_buffer, assert — on the order of 1800 lines).
Nothing there reaches into Xenia's GPU abstraction, its command processor or its
window system.

The back ends each carry exactly one dependency on Xenia's UI layer:

- the SPIR-V back end includes `ui/vulkan/vulkan_device.h`
- the DXBC back end includes `ui/graphics_provider.h`

That sounds worse than it is. The SPIR-V translator uses its Vulkan device for
**capability queries only** — thirteen `properties()` calls and one
`extensions()` call. Replacing it with a small struct we populate from our own
device is a shim, not a port.

## Recommendation

Take the SPIR-V path. Vulkan is the right target on this machine, and the
back end's only coupling is the capability shim described above.

Subset to compile from the fork (the rest of Xenia is not built):

- `gpu/`: `ucode`, `xenos`, `shader`, `shader_translator`, `spirv_builder`,
  `spirv_shader_translator*`, `registers` + `register_table.inc`,
  `texture_info*`, `texture_util`, `texture_address`
- `base/`: `math`, `memory`, `byte_order`, `string_buffer`, `assert`, and
  whatever platform header those pull in
- ours: a `VulkanDevice`-shaped shim exposing `properties()` and `extensions()`

Only these translation units get compiled into our target. The submodule brings
the whole tree, but nothing links Xenia's GPU abstraction, command processor or
window system.

## What this does and does not change

It removes the largest single unknown from the estimate. Writing a Xenos
microcode translator from scratch was the reason "small weeks" was the floor for
a first frame; that reason is gone, and integration is a smaller and much better
understood job than authorship.

It changes nothing else. The seam still has to be hooked, resources still arrive
as precomputed hardware descriptors carrying tiled formats and physical
addresses, and the EDRAM/resolve model is still ours to deal with. Translating
shaders correctly does not draw anything by itself.

And it does not touch the finding that decides sequencing: in the post-load
state the title calls no D3D entry points at all. A shader translator cannot fix
a title that is not drawing, and building the backend first would leave us
unable to tell a broken backend from a game that never called it.
