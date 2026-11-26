# MillSaph

OFX plugin (Flame, Nuke, …) for macOS, Linux and Windows.
Plugin code is plain C; the effects compute in Halide (`src/halide/`), compiled at build time
for the CPU and the GPU. The C code describes the effects and runs their Halide kernels in
order.

## Build

Needs CMake 3.28+ (Halide's CMake package) and a C and C++ compiler: Xcode command line
tools on macOS, `gcc` on Linux, Visual Studio 2022 ("Desktop development with C++") on
Windows.

```sh
./build.sh        # macOS / Linux
build.bat         # Windows
```

The result is in `build/mac/`, `build/lin/` or `build/win/`, for the machine you built on:

- `release/`: what is shipped, `ofx/MillSaph.ofx.bundle`, and `ae/` (below) when the After
  Effects SDK is there;
- `bin/`: the programs to run here (`test_host`, `millsaph_bench`);
- `cmake/`: CMake's work.

The first build downloads into `third_party/` (see `cmake/deps.cmake`): the OFX
headers, and Halide 21 prebuilt for the build machine (about 170 MB, LLVM
included). Later builds work offline. Building needs a C++ compiler too, only for the
Halide effects (`src/halide/`). On macOS the plugin needs macOS 12 or later.

On Linux (Rocky 9; Rocky 8 is too old: Halide needs glibc 2.34):

- CMake 3.28 or newer (Halide's CMake package needs it; Rocky's own is 3.26): e.g. Kitware's
  prebuilt release, or `pip install cmake`.
- GCC 12 or newer for the build (Halide's library needs its C++ runtime):
  `dnf install gcc-toolset-12`, then `scl enable gcc-toolset-12 ./build.sh`.
- zlib for the build (Halide's static library uses it): `dnf install zlib-devel`.

## GPU

Effects with a GPU version try, at render time, each GPU API of the system in this order,
then the CPU (`src/engine/run.c`): Metal on macOS; CUDA on Linux; CUDA then Direct3D 12 on
Windows. Only the driver is needed (NVIDIA for CUDA). An API that does not work (no driver,
no device) is not tried again in that session; the terminal and `~/millsaph.log` (in the
home folder; Windows: `%TEMP%\millsaph.log`) say
`[MillSaph] CUDA not available ..., trying the next`, and `[MillSaph] running on Metal` once
for the API the effects run on. The first 5000 renders of a session also log where their
time goes, their settings and the API they ran on. The MS_Test effect shows
it in its "Runs on" line.

GPU versions: BandPass (large blurs at the same cost as small ones); EdgeAwareBlur Sharp,
Softer and Soft, with their Edge Smooth (Soft where the profile says so: on an M1 Pro its
CPU is faster); Distort (its Enable GPU off forces the CPU); Mosaic, with its Smooth Colors; Glow; EdgeBlur; RackDefocus (a convolution by FFT); Glint; Glare (its style's elements, then a convolution by FFT); LensFlare (its lens's elements drawn along the flare's axis, textured by the lens's own images or by MillSaph's generated ones (an element's "image": {"generate": "dirt", "glint", "ramp" or "iris", "seed", "amount", "scale", "detail", "sides", "texels"}: generated on the CPU before the rest), its Preset menu (MillSaph's own lenses, presets/lensflare, written by tools/presets/make_lensflare_presets.py and compiled in), its Edit Lens button (MillSaph's lens editor, src/editor: millsaph_lens_editor, built into the bundle next to the plugin with Dear ImGui and GLFW (MILLSAPH_EDITOR), run and waited for; what it applies becomes the Lens text; OFX hosts for now), its viewer overlay (Scale Widths and Rel Heights round the hotspot, the hotspot and the pivot, dragged), its Atmosphere and Diffraction Glow; with an Atmosphere or a Flicker that moves, or the Motion Blur, it changes over time and the hosts render every frame, else they keep it). JpegDamage changes over time with Jitter Frames only. EdgeAwareBlur without blur, or
with an Edge Threshold of 0, tells the host that it leaves the Source as it is: the host
skips it, and so does Mosaic with a Mix With Source of 1, and Glow without glow (Brightness
or Color 0) in Add, Screen or Difference, and EdgeBlur without blur (Blur Width under a pixel,
or Edge Strength 0), and RackDefocus without a lens nor a change of the colors (or a Mix
With Source of 1), and Glare without glare (Size, Brightness or Scale Colors 0) over the whole
Source, and LensFlare without flare (Scale Widths, Brightness or Color 0) screened or added
over the whole Source. The GPU memory of a render's images is kept for the
next renders, up to 1/16 of the machine's memory (at most 2 GB). Which version is faster
depends on the GPU: `src/effects/profile.c` holds those choices per API (measured on an M1
Pro; `tools/perf/` measures them on another machine).

Viewer overlays (OFX hosts; src/ofx/overlay.c, the host's Draw Suite or OpenGL, each effect's
shapes in its MsEffect.overlay): the widths of BandPass, EdgeAwareBlur, EdgeBlur, Glow,
RackDefocus and Glare as circles round the frame's middle, with arrows for their relative
width and height; Glint's rays and sizes; LensFlare's Scale Widths round the hotspot, the
hotspot and the pivot; FlickerMatch's rectangle. The shape under the pen is lit (yellow);
dragging it edits the parameters it is drawn from. Each has its Show checkbox.

Images: RGBA in 32-bit or 16-bit floats, whichever the host works in (a 16-bit project stays
16-bit); the effects compute in 32-bit floats, the 16-bit images converted on the GPU (less to
copy). With CUDA, the host's images are page-locked for the copies to and from the GPU (about
twice as fast). An input plugged into the same image as the Source is copied once.

Images narrower or shorter than 16 pixels are not supported yet: the render fails with a
message.

## Presets

Every effect has a Load Preset button: it opens MillSaph's preset window
(`millsaph_preset_browser`, next to the plugin: thumbnails, search, tags) and applies the
preset picked there (every parameter back to its default, then the preset's; `src/common/preset.c`).
The presets are files, `presets/<effect>/*.json` (`{"name", "tags", "params"}`), copied into
the bundle's `Contents/Resources/presets/` with a thumbnail each, rendered at build time by
`millsaph_thumbnails` (`tools/presets/thumbnails.cpp`) on MillSaph's own test picture.
Most are written by `tools/measure/convert_presets.py` (the reference's presets in MillSaph's
settings, `tools/measure/README.md`); the Lens Flare's MillSaph lenses by
`tools/presets/make_lensflare_presets.py`.

## Build settings

All settings are environment variables. `build.sh` / `build.bat` set them to the
defaults below unless they are already set (in production they come from rez).

| Variable | Default | What |
|---|---|---|
| `MILLSAPH_THIRD_PARTY_DIR` | `third_party/` | OFX headers and Halide (downloaded if missing) |
| `MILLSAPH_OFX_VERSION` | `OFX_Release_1.5.1` | OpenFX release to download |

## Install

Copy the whole `MillSaph.ofx.bundle` folder to the OFX plugin folder (`./install.sh` does it on
macOS and Linux), then restart the host:

| System | Folder |
|---|---|
| macOS | `/Library/OFX/Plugins/` |
| Linux | `/usr/OFX/Plugins/` |
| Windows | `C:\Program Files\Common Files\OFX\Plugins\` |

## After Effects (experimental)

`build.sh` / `build.bat` also build the After Effects plugin (every effect but MS_Test) with
Adobe's After Effects SDK: its zips for macOS and Windows
(27.0, from developer.adobe.com, Adobe account needed) are in `third_party/`, and the build
unzips the one for its system. A newer SDK: put its zip there, named as Adobe names it
(`AfterEffectsSDK_<version>_mac.zip` / `_win.zip`); the newest is used.

- macOS: `build/mac/release/ae/MillSaph.plugin` (arm64 + x86_64), to copy to
  `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`.
- Windows: `build/win/release/ae/MillSaph.aex`, to copy to
  `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`.

The effects are in the MillSaph category; widths are shown as a percentage of the layer
width, colours with After Effects' colour picker (read in floats).

Known limitations: built against the SDK (27.0 on macOS), but never run in After Effects.
To check there, see the open points at the top of `src/ae/plugin.c` (straight or
premultiplied alpha, the buffers' origin). The images are converted to packed floats on the
CPU (8, 16 or 32 bits), without the OFX plugin's in-place reads and 16-bit transfers.

## Source layout

| Folder | What |
|---|---|
| `src/ofx/` | The OFX adapter: it turns each effect into an OFX plugin (`plugin.c`, `image.c`, `host.c`). It reads and writes the host's images in place where it can. |
| `src/ae/` | The After Effects adapter (macOS, Windows): `plugin.c`, and `pipl.r`, which lists the effects for After Effects. |
| `src/effects/` | The effects (`ms_*.c`): parameters and inputs, then the kernels each render runs, in order, and on which API (`profile.c`). The list of effects: `list.def` (one line each, read by `effects.c`, the OFX and After Effects adapters and the PiPL). |
| `src/engine/` | What the effects run on (`engine.h`): kernels called by name, images, and `ms_run`, which runs an effect's kernels on the first API that works and keeps their GPU memory between renders. |
| `src/halide/` | The kernels, in Halide (C++): compiled at build time for the CPU and each GPU API (Metal on macOS, CUDA on Linux, CUDA / Direct3D 12 on Windows). `CMakeLists.txt` lists them (`ms_kernel`) and generates their table. |
| `src/common/` | Shared by every layer, without any host or GPU API: the effect description (`effect.h`), the Mask parameters, a mutex. |

Includes name their folder (`#include "engine/engine.h"`), so each file shows which
layers it uses. Effects never include `ofx/`.

A new kernel: a generator in `src/halide/`, a `ms_kernel(...)` line in `CMakeLists.txt`,
then `MS_KERNEL(MS_K_<NAME>, api, MS_IMAGE("input", ...), MS_INT(...), ...)` in the effect:
arguments by the names the generator gives them, checked at each call (a wrong name or type
fails the render and says why in the log).

## Development

The checks use LLVM's tools (`clang-format`, `clang-tidy`, `clangd`). On macOS:
`brew install llvm`, then add `/opt/homebrew/opt/llvm/bin` to your `PATH`.

| Command | What |
|---|---|
| `tools/format.sh` | Formats every C file (style in `.clang-format`: LLVM, 4 spaces, 100 columns). `--check` only lists what is not formatted. |
| `tools/tidy.sh` | Static checks with clang-tidy (`.clang-tidy`): likely bugs, found without running the code. |
| `tools/sanitize.sh` | Builds with AddressSanitizer + UndefinedBehaviorSanitizer and renders every effect outside any host (`tools/test_host.c`). Stops at the line of a memory error. |
| `tools/perf/` | Timings and GPU checks outside any host and in Nuke (`tools/perf/README.md`). |
| `tools/measure/` | Measuring an effect in Nuke to recreate it: parameters, probe images, renders, comparisons (`tools/measure/README.md`). |

Editors: `clangd` reads `build/compile_commands.json` (written by `./build.sh`), shows
errors and clang-tidy warnings as you type, and formats on save with `.clang-format`.
