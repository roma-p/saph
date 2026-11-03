# MillSaph

OFX plugin (Flame, Nuke, …) for macOS, Linux and Windows.
Plugin code is plain C; the effects compute in Halide (`src/halide/`), compiled at build time
for the CPU and the GPU. The C code describes the effects and runs their Halide kernels in
order.

## Build

Needs CMake 3.18+ and a C compiler (Xcode command line tools on macOS,
`gcc` on Linux, Visual Studio on Windows).

```sh
./build.sh        # macOS / Linux
build.bat         # Windows
```

The result is `build/MillSaph.ofx.bundle`, for the machine you built on.

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
CPU is faster); Distort (its Enable GPU off forces the CPU). EdgeAwareBlur without blur, or
with an Edge Threshold of 0, tells the host that it leaves the Source as it is: the host
skips it. The GPU memory of a render's images is kept for the
next renders, up to 1/16 of the machine's memory (at most 2 GB). Which version is faster
depends on the GPU: `src/effects/profile.c` holds those choices per API (measured on an M1
Pro; `tools/perf/` measures them on another machine).

Images: RGBA in 32-bit or 16-bit floats, whichever the host works in (a 16-bit project stays
16-bit); the effects compute in 32-bit floats, the 16-bit images converted on the GPU (less to
copy). With CUDA, the host's images are page-locked for the copies to and from the GPU (about
twice as fast). An input plugged into the same image as the Source is copied once.

Images narrower or shorter than 16 pixels are not supported yet: the render fails with a
message.

## Build settings

All settings are environment variables. `build.sh` / `build.bat` set them to the
defaults below unless they are already set (in production they come from rez).

| Variable | Default | What |
|---|---|---|
| `MILLSAPH_THIRD_PARTY_DIR` | `third_party/` | OFX headers and Halide (downloaded if missing) |
| `MILLSAPH_OFX_VERSION` | `OFX_Release_1.5.1` | OpenFX release to download |

## Install

Copy the whole `MillSaph.ofx.bundle` folder to the OFX plugin folder, then restart the host:

| System | Folder |
|---|---|
| macOS | `/Library/OFX/Plugins/` |
| Linux | `/usr/OFX/Plugins/` |
| Windows | `C:\Program Files\Common Files\OFX\Plugins\` |

## After Effects (Windows, experimental)

`build.bat` also builds `build/ae/MillSaph.aex` (MS_BandPass, MS_EdgeAwareBlur,
MS_Distort) when Adobe's After Effects SDK is present: download it from
developer.adobe.com (needs an Adobe account) and unzip it so that
`third_party/AfterEffectsSDK/Examples` exists. Copy `MillSaph.aex` to
`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`. The effects are in the
MillSaph category; widths are shown as a percentage of the layer width.

Not yet tried in After Effects: written from the SDK documentation and checked only by
compiling against a stand-in of its headers. See the open points at the top of
`src/ae/plugin.c`.

## Source layout

| Folder | What |
|---|---|
| `src/ofx/` | The OFX adapter: it turns each effect into an OFX plugin (`plugin.c`, `image.c`, `host.c`). It reads and writes the host's images in place where it can. |
| `src/ae/` | The After Effects adapter (Windows): `plugin.c`, and `pipl.r`, which lists the effects for After Effects. |
| `src/effects/` | The effects (`ms_*.c`): parameters and inputs, then the kernels each render runs, in order, and on which API (`profile.c`). The list of effects: `effects.c`. |
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

Editors: `clangd` reads `build/compile_commands.json` (written by `./build.sh`), shows
errors and clang-tidy warnings as you type, and formats on save with `.clang-format`.
