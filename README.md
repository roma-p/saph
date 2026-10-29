# MillSaph

OFX plugin (Flame, Nuke, …) for macOS, Linux and Windows.
Plugin code is plain C; effects run on the GPU as GLSL shaders.

## Build

Needs CMake 3.18+ and a C compiler (Xcode command line tools on macOS,
`gcc` on Linux, Visual Studio on Windows).

```sh
./build.sh        # macOS / Linux
build.bat         # Windows
```

The result is `build/MillSaph.ofx.bundle`, for the machine you built on.

The first build downloads the OFX and OpenGL headers into `third_party/`
(see `cmake/deps.cmake`). Later builds
work offline.

## Build settings

All settings are environment variables. `build.sh` / `build.bat` set them to the
defaults below unless they are already set (in production they come from rez).

| Variable | Default | What |
|---|---|---|
| `MILLSAPH_THIRD_PARTY_DIR` | `third_party/` | OFX and OpenGL headers (downloaded if missing) |
| `MILLSAPH_OFX_VERSION` | `OFX_Release_1.5.1` | OpenFX release to download |

## Install

Copy the whole `MillSaph.ofx.bundle` folder to the OFX plugin folder, then restart the host:

| System | Folder |
|---|---|
| macOS | `/Library/OFX/Plugins/` |
| Linux | `/usr/OFX/Plugins/` |
| Windows | `C:\Program Files\Common Files\OFX\Plugins\` |

## Source layout

| Folder | What |
|---|---|
| `src/common/` | Shared by every layer, without any host or GPU API: the effect description (`effect.h`), the 3-box blur, the Mask helpers, the CPU parallel loop. |
| `src/effects/` | The effects (`ms_*.c`), the list of effects (`effects.c`) and their CPU algorithms. |
| `src/gpu/` | The GPU runner (`gpu.c`) and its backends: OpenGL (`gl.c`, contexts in `gl_mac.c` / `gl_linux.c`). |
| `src/ofx/` | The OFX adapter: it turns each effect into an OFX plugin (`plugin.c`, `image.c`, `host.c`). |
| `src/glsl/` | The shaders, embedded in the plugin at build time. |

Includes name their folder (`#include "common/blur.h"`), so each file shows which
layers it uses. Effects never include `ofx/`.

## Development

The checks use LLVM's tools (`clang-format`, `clang-tidy`, `clangd`). On macOS:
`brew install llvm`, then add `/opt/homebrew/opt/llvm/bin` to your `PATH`.

| Command | What |
|---|---|
| `tools/format.sh` | Formats every C file (style in `.clang-format`: LLVM, 4 spaces, 100 columns). `--check` only lists what is not formatted. |
| `tools/tidy.sh` | Static checks with clang-tidy (`.clang-tidy`): likely bugs, found without running the code. |
| `tools/sanitize.sh` | Builds with AddressSanitizer + UndefinedBehaviorSanitizer and renders every effect outside any host (`tools/test_host.c`). Stops at the line of a memory error. |
| `tools/gpu_check.sh` | Starts the GPU outside any host and runs one shader. |

Editors: `clangd` reads `build/compile_commands.json` (written by `./build.sh`), shows
errors and clang-tidy warnings as you type, and formats on save with `.clang-format`.
