cmake_minimum_required(VERSION 3.19) # string(JSON) for the presets

# macOS: one binary for Apple Silicon and Intel. Only on macOS: Halide reads this setting on
# every system and would try to build (and lipo) two processors elsewhere too.
if(CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "") # Halide's Intel code needs 12
else()
  unset(CMAKE_OSX_ARCHITECTURES CACHE) # in case a build folder still has it
endif()
# The macOS SDK, named in build/compile_commands.json so clangd and clang-tidy find it.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_SYSROOT)
  execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(CMAKE_OSX_SYSROOT "${sdk}" CACHE PATH "")
endif()

foreach(var MILLSAPH_VERSION MILLSAPH_THIRD_PARTY_DIR MILLSAPH_OFX_VERSION)
  if("$ENV{${var}}" STREQUAL "")
    message(FATAL_ERROR "${var} is not set. Build with build.sh / build.bat, or from the rez environment.")
  endif()
endforeach()
project(MillSaph VERSION $ENV{MILLSAPH_VERSION} LANGUAGES C)

file(TO_CMAKE_PATH "$ENV{MILLSAPH_THIRD_PARTY_DIR}" third_party)  # also turns Windows \ into /

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# build/compile_commands.json: how each file is compiled, for clangd and clang-tidy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug build that stops at memory errors and undefined behaviour (tools/sanitize.sh).
option(MILLSAPH_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MILLSAPH_EDITOR "Build the Lens Flare editor (src/editor; Dear ImGui, GLFW)" ON)
if(MILLSAPH_SANITIZE)
  set(CMAKE_BUILD_TYPE Debug)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

include(cmake/deps.cmake)

# Halide (cmake/deps.cmake downloads it): effects written in Halide (src/halide/*_generator.cpp,
# the only C++) are compiled at build time into C functions, for the CPU and the GPU.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Halide's shared library needs a newer libstdc++ (GCC 12) than Rocky 9 has; linked
  # statically (with gcc-toolset-12 or newer), the generator runs there.
  find_package(Halide 21 REQUIRED CONFIG COMPONENTS static PATHS ${third_party}/halide NO_DEFAULT_PATH)
else()
  find_package(Halide 21 REQUIRED CONFIG PATHS ${third_party}/halide NO_DEFAULT_PATH)
endif()
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)

# millsaph_core: the effects and what they need, without any host API (src/common,
# src/effects, src/engine, and the kernels below). The host adapters link it: src/ofx makes
# the OFX plugin, src/ae the After Effects plugin (macOS, Windows). Includes name the folder:
# "engine/engine.h".
file(GLOB core_sources CONFIGURE_DEPENDS src/common/*.c src/effects/*.c src/engine/*.c)
add_library(millsaph_core STATIC ${core_sources} ${CMAKE_BINARY_DIR}/generated/kernels.c)
set_target_properties(millsaph_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(millsaph_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)

find_package(Threads REQUIRED)
target_link_libraries(millsaph_core PUBLIC Threads::Threads)

if(APPLE)
  set(platform MacOS)
elseif(WIN32)
  set(platform Win64)
  # M_PI in <math.h>; no warnings for the standard C string functions (MSVC).
  target_compile_definitions(millsaph_core PUBLIC _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
else()
  set(platform Linux-x86-64)
  # libm: the kernels' CPU code calls it (sin, cos, sincos), the plugin needs it itself.
  target_link_libraries(millsaph_core PUBLIC ${CMAKE_DL_LIBS} m)
endif()

# GPU APIs, in the order effects try them at render time (src/engine/run.c), then the CPU.
# Halide generates the GPU code itself: only the driver is needed, on the machine rendering.
# Halide names: metal, cuda, vulkan, d3d12compute.
if(APPLE)
  set(gpu_apis metal)
elseif(WIN32)
  set(gpu_apis cuda d3d12compute)
else()
  # Not Vulkan: on an RTX A6000 it was slower than the CPU (about 350 ms per HD frame against
  # 45 to 90) and Distort's results were wrong; after CUDA, the CPU. (The engine still takes
  # it: `cuda vulkan` here builds it again.)
  set(gpu_apis cuda)
endif()

file(GLOB generators CONFIGURE_DEPENDS src/halide/*_generator.cpp)
add_halide_generator(millsaph_generators SOURCES ${generators})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The static Halide library (see find_package above) leaves zlib to us (Rocky: zlib-devel).
  find_package(ZLIB REQUIRED)
  target_link_libraries(millsaph_generators PRIVATE ZLIB::ZLIB)
endif()
if(APPLE)
  # The generator only runs here, at build time: this machine's processor, and the macOS
  # version Halide's library was built for (the plugin itself stays universal, macOS 12).
  set_target_properties(millsaph_generators PROPERTIES OSX_ARCHITECTURES ${CMAKE_HOST_SYSTEM_PROCESSOR})
  target_compile_options(millsaph_generators PRIVATE -mmacosx-version-min=13.1)
  target_link_options(millsaph_generators PRIVATE -mmacosx-version-min=13.1)
endif()
if(WIN32)
  # The generator runs at build time: Halide's DLL next to it, where Windows looks first.
  add_custom_command(TARGET millsaph_generators POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:millsaph_generators>
            $<TARGET_FILE_DIR:millsaph_generators>
    COMMAND_EXPAND_LISTS)
endif()
add_halide_runtime(millsaph_halide_runtime) # shared; gets the GPU support its users need

# The Halide targets of a GPU version: the build's processor(s) with the API's feature.
# A universal macOS build (two processors) takes the feature per processor.
function(millsaph_gpu_targets out api)
  list(LENGTH CMAKE_OSX_ARCHITECTURES processors)
  if(APPLE AND processors GREATER 1)
    set(${out} TARGETS cmake FEATURES[arm-64-osx] ${api} FEATURES[x86-64-osx] ${api} PARENT_SCOPE)
  else()
    set(${out} TARGETS cmake-${api} PARENT_SCOPE)
  endif()
endfunction()

# Kernels: the Halide pipelines the effects call by name (src/engine/engine.h, ms_kernel).
#   ms_kernel(<name> GENERATOR <generator> [PARAMS <generator params>] [CPU] [GPU])
# builds a version for the CPU and / or each GPU API (libraries ms_k_<name>_<api>); the
# effects call it as MS_K_<NAME>.
function(ms_kernel name)
  cmake_parse_arguments(arg "CPU;GPU" "GENERATOR" "PARAMS" ${ARGN})
  set(apis)
  if(arg_GPU)
    list(APPEND apis ${gpu_apis})
  endif()
  if(arg_CPU)
    list(APPEND apis cpu)
  endif()
  foreach(api ${apis})
    if(api STREQUAL cpu)
      set(targets TARGETS cmake)
    else()
      millsaph_gpu_targets(targets ${api})
    endif()
    add_halide_library(ms_k_${name}_${api} FROM millsaph_generators GENERATOR ${arg_GENERATOR}
                       ${targets} PARAMS ${arg_PARAMS} USE_RUNTIME millsaph_halide_runtime)
    target_link_libraries(millsaph_core PUBLIC ms_k_${name}_${api})
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ms_kernels ${name})
  set_property(GLOBAL PROPERTY ms_kernel_apis_${name} ${apis})
endfunction()

# Shared by the effects (src/halide/mask_generator.cpp), and MS_Test.
ms_kernel(mask GENERATOR ms_mask CPU GPU)
ms_kernel(gain GENERATOR ms_gain CPU GPU)
# The host's 16-bit float images to float and back (src/engine/run.c).
ms_kernel(to_float GENERATOR ms_to_float CPU GPU)
ms_kernel(to_half GENERATOR ms_to_half CPU GPU)
# MS_BandPass. On the GPU, the staged version (windows in on-chip memory) for most blur sizes.
ms_kernel(bandpass GENERATOR ms_bandpass CPU GPU)
ms_kernel(bandpass_staged GENERATOR ms_bandpass PARAMS boxes=staged GPU)
ms_kernel(bandpass_prefix GENERATOR ms_bandpass PARAMS boxes=prefix GPU) # large blurs
# The 3-box blur of an RGBA image (MS_Mosaic's Smooth Colors); prefix: for large blurs.
ms_kernel(blur GENERATOR ms_blur CPU GPU)
ms_kernel(blur_prefix GENERATOR ms_blur PARAMS boxes=prefix GPU)
# MS_Mosaic.
ms_kernel(mosaic GENERATOR ms_mosaic CPU GPU)
# MS_JpegDamage: the JPEG round trip.
ms_kernel(jpeg_damage GENERATOR ms_jpeg_damage CPU GPU)
# MS_PixelSort: the lines' values and spans, the bitonic network's passes, the result; the
# image at the Sort Resolution.
ms_kernel(pixel_sort_prep GENERATOR ms_pixel_sort_prep CPU GPU)
ms_kernel(pixel_sort_step GENERATOR ms_pixel_sort_step CPU GPU)
ms_kernel(pixel_sort_block GENERATOR ms_pixel_sort_block CPU GPU)
ms_kernel(pixel_sort_out GENERATOR ms_pixel_sort_out CPU GPU)
ms_kernel(pixel_sort_down GENERATOR ms_pixel_sort_down CPU GPU)
# MS_FlickerMatch, MS_FlickerMatchColor: the Match input's level in the rectangle, the gains.
ms_kernel(flicker_match GENERATOR ms_flicker_match CPU GPU)
# MS_Glow: the thresholded Source blurred, then combined.
ms_kernel(glow_blur GENERATOR ms_glow_blur CPU GPU)
ms_kernel(glow_combine GENERATOR ms_glow_combine CPU GPU)
# MS_EdgeBlur: the edges of the Matte, then the Source blurred where they are.
ms_kernel(edge_blur_edges GENERATOR ms_edge_blur_edges CPU GPU)
ms_kernel(edge_blur GENERATOR ms_edge_blur CPU GPU)
# MS_Glint: the thresholded Source, its rays one direction at a time, then combined.
ms_kernel(glint_source GENERATOR ms_glint_source CPU GPU)
ms_kernel(glint_ray_x GENERATOR ms_glint_ray PARAMS direction=0 CPU GPU)
ms_kernel(glint_ray_y GENERATOR ms_glint_ray PARAMS direction=1 CPU GPU)
ms_kernel(glint_ray_up GENERATOR ms_glint_ray PARAMS direction=2 CPU GPU)
ms_kernel(glint_ray_down GENERATOR ms_glint_ray PARAMS direction=3 CPU GPU)
ms_kernel(glint_combine GENERATOR ms_glint_combine CPU GPU)
ms_kernel(glint_combine_blur GENERATOR ms_glint_combine PARAMS blurred=true CPU GPU)
# MS_Glare: the style's kernel, the spots (connected regions), their impulses, the finish
# (with Glint's ms_glint_source, the transforms and RackDefocus's product).
foreach(name glare_kernel glare_label_init glare_label_round glare_spots glare_half glare_finish)
  ms_kernel(${name} GENERATOR ms_${name} CPU GPU)
endforeach()
# MS_LensFlare: the occlusion at the hotspot, the elements drawn along the axis, the
# Diffraction Glow, the finish.
foreach(name lensflare_occlusion lensflare_draw lensflare_glow lensflare_finish)
  ms_kernel(${name} GENERATOR ms_${name} CPU GPU)
endforeach()
# (its own textures, generated on the CPU before the rest)
ms_kernel(lensflare_texture GENERATOR ms_lensflare_texture CPU)
# The effects that convolve by FFT (src/effects/fft.c): one stage of a transform.
ms_kernel(fft_stage GENERATOR ms_fft_stage CPU GPU)
# MS_RackDefocus: the lens and the Source transformed, multiplied, transformed back.
foreach(name rack_defocus_prepare rack_defocus_kernel rack_defocus_multiply rack_defocus_finish)
  ms_kernel(${name} GENERATOR ms_${name} CPU GPU)
endforeach()
# MS_Distort.
foreach(name distort_shift distort_levels distort_warp)
  ms_kernel(${name} GENERATOR ms_${name} CPU GPU)
endforeach()
# MS_EdgeAwareBlur, the Domain Transform filter: Sharp and Softer have a GPU version (in
# float, per block of a row) besides the CPU one (in double, over whole lines), and so has
# their preparation (Edge Smooth, distances); Soft, a recursion along whole lines, too (one
# GPU thread per line).
foreach(name dt_distances dt_edge_smooth dt_soft_weights dt_soft)
  ms_kernel(${name} GENERATOR ms_${name} CPU GPU)
endforeach()
foreach(name dt_largest dt_sharp dt_softer)
  ms_kernel(${name} GENERATOR ms_${name} CPU)
endforeach()
ms_kernel(dt_transpose GENERATOR ms_dt_transpose GPU)
ms_kernel(dt_sharp_gpu GENERATOR ms_dt_sharp_gpu GPU)
ms_kernel(dt_sharp_short_gpu GENERATOR ms_dt_sharp_gpu PARAMS tile=64 chunk=8 GPU) # margins up to 128
ms_kernel(dt_softer_gpu GENERATOR ms_dt_softer_gpu GPU)
# Windows too wide for a block's on-chip memory: running sums per row in GPU memory.
ms_kernel(dt_sharp_rows_gpu GENERATOR ms_dt_sharp_gpu PARAMS rows=true GPU)
ms_kernel(dt_softer_rows_gpu GENERATOR ms_dt_softer_gpu PARAMS rows=true GPU)

# The kernel table (src/engine/internal.h): generated/kernels.h names the kernels,
# generated/kernels.c gives each one's version per API.
set(api_enum_cpu MS_API_CPU)
set(api_enum_metal MS_API_METAL)
set(api_enum_cuda MS_API_CUDA)
set(api_enum_vulkan MS_API_VULKAN)
set(api_enum_d3d12compute MS_API_D3D12)
get_property(kernels GLOBAL PROPERTY ms_kernels)
set(names "")
set(includes "")
set(entries "")
foreach(name ${kernels})
  string(TOUPPER ${name} upper)
  string(APPEND names "    MS_K_${upper},\n")
  string(APPEND entries "    [MS_K_${upper}] = {\"${name}\", {")
  get_property(apis GLOBAL PROPERTY ms_kernel_apis_${name})
  foreach(api ${apis})
    string(APPEND includes "#include \"ms_k_${name}_${api}.h\"\n")
    string(APPEND entries "[${api_enum_${api}}] = {ms_k_${name}_${api}_argv, ms_k_${name}_${api}_metadata}, ")
  endforeach()
  string(APPEND entries "}},\n")
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/kernels.h @ONLY CONTENT
"// Generated by CMakeLists.txt (ms_kernel): the kernels.\n\n#pragma once\n\ntypedef enum {\n${names}    MS_KERNEL_COUNT,\n} MsKernel;\n")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/kernels.c @ONLY CONTENT
"// Generated by CMakeLists.txt (ms_kernel): each kernel's version per API.\n\n#include \"engine/internal.h\"\n${includes}\nconst MsKernelEntry ms_kernel_table[MS_KERNEL_COUNT] = {\n${entries}};\n")

# MillSaph's own Lens Flare presets (presets/lensflare/*.json, tools/presets): compiled in,
# generated/lensflare_presets.h names them (their "preset-name") and holds their texts.
file(GLOB lensflare_presets CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/presets/lensflare/*.json)
set(preset_names "")
set(preset_texts "")
foreach(path ${lensflare_presets})
  file(READ ${path} text)
  string(JSON name GET ${text} value metadata preset-name)
  string(REPLACE "\\" "\\\\" text "${text}")
  string(REPLACE "\"" "\\\"" text "${text}")
  string(REPLACE "\n" "\\n\"\n    \"" text "${text}")
  string(APPEND preset_names "    \"${name}\",\n")
  string(APPEND preset_texts "    \"${text}\",\n")
endforeach()
list(LENGTH lensflare_presets preset_count)
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/lensflare_presets.h @ONLY CONTENT
"// Generated by CMakeLists.txt from presets/lensflare: MillSaph's Lens Flare presets.\n\n#pragma once\n\n#define MS_LENSFLARE_PRESETS ${preset_count}\n// (the Preset menu: the Lens text first, then the presets)\nstatic const char *const ms_lensflare_preset_menu[1 + MS_LENSFLARE_PRESETS] = {\n    \"Lens Text\",\n${preset_names}};\nstatic const char *const ms_lensflare_preset_texts[MS_LENSFLARE_PRESETS] = {\n${preset_texts}};\n")

# MILLSAPH_API_METAL, _CUDA, _VULKAN, _D3D12: the GPU APIs built (src/engine/run.c).
foreach(api ${gpu_apis})
  string(TOUPPER ${api} name)
  string(REPLACE "D3D12COMPUTE" "D3D12" name ${name})
  target_compile_definitions(millsaph_core PRIVATE MILLSAPH_API_${name})
endforeach()

function(millsaph_warnings target)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -Wall -Wextra -fvisibility=hidden)
  endif()
endfunction()
millsaph_warnings(millsaph_core)

# ---------------------------------------------------------------- OFX plugin

file(GLOB ofx_sources CONFIGURE_DEPENDS src/ofx/*.c)
add_library(MillSaph MODULE ${ofx_sources})
target_include_directories(MillSaph PRIVATE ${third_party}/openfx/include)
target_link_libraries(MillSaph PRIVATE millsaph_core)
# (overlays drawn with OpenGL where the host has no Draw Suite)
find_package(OpenGL REQUIRED)
target_link_libraries(MillSaph PRIVATE OpenGL::GL)
millsaph_warnings(MillSaph)

set(bundle ${CMAKE_BINARY_DIR}/MillSaph.ofx.bundle)
set_target_properties(MillSaph PROPERTIES
  PREFIX ""
  SUFFIX ".ofx"
  LIBRARY_OUTPUT_DIRECTORY ${bundle}/Contents/${platform}
  LIBRARY_OUTPUT_DIRECTORY_RELEASE ${bundle}/Contents/${platform}  # Visual Studio
)

if(APPLE)
  configure_file(cmake/Info.plist.in ${bundle}/Contents/Info.plist @ONLY)
  # Apple Silicon only loads signed code; "-" is a local (ad-hoc) signature.
  add_custom_command(TARGET MillSaph POST_BUILD COMMAND codesign --force --sign - ${bundle})
endif()

# ---------------------------------------------------------------- benchmark

# Every effect on every API of this machine (tools/perf/bench.c, run by tools/perf/collect.sh).
# MillSaph's lens editor (src/editor): MS_LensFlare's Edit Lens runs it, so it is built next
# to the plugin's binary in the bundle. Dear ImGui with its GLFW and OpenGL 3 backends, GLFW
# built here (X11 on Linux).
if(MILLSAPH_EDITOR)
  enable_language(CXX)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  add_subdirectory(${third_party}/glfw ${CMAKE_BINARY_DIR}/glfw EXCLUDE_FROM_ALL)
  set(imgui ${third_party}/imgui)
  add_executable(millsaph_lens_editor src/editor/main.cpp src/editor/lens.cpp src/editor/preview.cpp
    ${imgui}/imgui.cpp ${imgui}/imgui_draw.cpp ${imgui}/imgui_tables.cpp ${imgui}/imgui_widgets.cpp
    ${imgui}/backends/imgui_impl_glfw.cpp ${imgui}/backends/imgui_impl_opengl3.cpp)
  set_target_properties(millsaph_lens_editor PROPERTIES CXX_STANDARD 17
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/MillSaph.ofx.bundle/Contents/${platform}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/MillSaph.ofx.bundle/Contents/${platform})
  target_include_directories(millsaph_lens_editor PRIVATE ${imgui} ${imgui}/backends ${third_party}/json)
  find_package(OpenGL REQUIRED)
  target_link_libraries(millsaph_lens_editor PRIVATE millsaph_core glfw OpenGL::GL)
  if(NOT WIN32 AND NOT APPLE)
    target_link_libraries(millsaph_lens_editor PRIVATE m)
  endif()
  add_dependencies(MillSaph millsaph_lens_editor)
endif()

# build/test_host: a minimal OFX host running the plugin outside a DCC (tools/test_host.c;
# tools/measure/ms_render.py, bitexact.sh). (dlopen: not on Windows.)
if(NOT WIN32)
  add_executable(test_host tools/test_host.c src/common/half.c)
  target_include_directories(test_host PRIVATE src ${third_party}/openfx/include)
  target_link_libraries(test_host PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(test_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

add_executable(millsaph_bench tools/perf/bench.c tools/perf/images.c tools/perf/transfer.c)
target_link_libraries(millsaph_bench PRIVATE millsaph_core)
if(NOT WIN32)
  target_link_libraries(millsaph_bench PRIVATE m)
endif()
millsaph_warnings(millsaph_bench)

# ---------------------------------------------------------------- After Effects plugin

# Windows (MSVC) and macOS. Needs the After Effects SDK, which Adobe distributes to Adobe
# accounts (developer.adobe.com): unzip it so that third_party/AfterEffectsSDK/Examples
# exists. Without it, only the OFX plugin is built.
set(ae_sdk ${third_party}/AfterEffectsSDK/Examples)
if(APPLE AND EXISTS ${ae_sdk}/Headers/AE_Effect.h)
  # build/ae/MillSaph.plugin: the module in Contents/MacOS, the PiPL resource (src/ae/pipl.r)
  # in Contents/Resources. Rez's own preprocessor has no macros with arguments: the C one
  # expands them first.
  set(ae_bundle ${CMAKE_BINARY_DIR}/ae/MillSaph.plugin)
  file(MAKE_DIRECTORY ${ae_bundle}/Contents/Resources)
  add_custom_command(
    OUTPUT ${ae_bundle}/Contents/Resources/MillSaph.rsrc
    COMMAND ${CMAKE_C_COMPILER} -E -P -x c -I${ae_sdk}/Headers -I${ae_sdk}/Resources
            ${CMAKE_SOURCE_DIR}/src/ae/pipl.r -o ${CMAKE_BINARY_DIR}/ae/pipl.rr
    COMMAND xcrun Rez -d __MACH__ -useDF -i ${ae_sdk}/Headers -i ${ae_sdk}/Resources
            -o ${ae_bundle}/Contents/Resources/MillSaph.rsrc ${ae_sdk}/Resources/AE_General.r
            ${CMAKE_BINARY_DIR}/ae/pipl.rr
    DEPENDS src/ae/pipl.r
    VERBATIM)
  add_library(MillSaphAE MODULE src/ae/plugin.c ${ae_bundle}/Contents/Resources/MillSaph.rsrc)
  target_include_directories(MillSaphAE PRIVATE ${ae_sdk}/Headers ${ae_sdk}/Headers/SP)
  target_link_libraries(MillSaphAE PRIVATE millsaph_core)
  millsaph_warnings(MillSaphAE)
  set_target_properties(MillSaphAE PROPERTIES
    OUTPUT_NAME MillSaph
    PREFIX ""
    SUFFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${ae_bundle}/Contents/MacOS
  )
  configure_file(cmake/AEInfo.plist.in ${ae_bundle}/Contents/Info.plist @ONLY)
  if(MILLSAPH_EDITOR) # the Lens Flare editor next to the module (Edit Lens)
    add_dependencies(MillSaphAE millsaph_lens_editor)
    add_custom_command(TARGET MillSaphAE POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy
                       $<TARGET_FILE:millsaph_lens_editor> ${ae_bundle}/Contents/MacOS/)
  endif()
  add_custom_command(TARGET MillSaphAE POST_BUILD COMMAND codesign --force --sign - ${ae_bundle})
elseif(WIN32 AND MSVC AND EXISTS ${ae_sdk}/Headers/AE_Effect.h)
  # The PiPL resource (src/ae/pipl.r) describes each effect to After Effects. Adobe's
  # PiPLtool turns it into a Windows resource, between two passes of the preprocessor.
  set(pipl ${CMAKE_BINARY_DIR}/ae)
  file(MAKE_DIRECTORY ${pipl})
  add_custom_command(
    OUTPUT ${pipl}/pipl.rc
    COMMAND ${CMAKE_C_COMPILER} /nologo /EP /P /Fi${pipl}/pipl.rr /I${ae_sdk}/Headers
            /I${ae_sdk}/Resources /Tc${CMAKE_SOURCE_DIR}/src/ae/pipl.r
    COMMAND ${ae_sdk}/Resources/PiPLtool.exe ${pipl}/pipl.rr ${pipl}/pipl.rrc
    COMMAND ${CMAKE_C_COMPILER} /nologo /EP /P /Fi${pipl}/pipl.rc /DMSWindows /Tc${pipl}/pipl.rrc
    DEPENDS src/ae/pipl.r
    WORKING_DIRECTORY ${pipl}
    VERBATIM)

  add_library(MillSaphAE MODULE src/ae/plugin.c ${pipl}/pipl.rc)
  target_include_directories(MillSaphAE PRIVATE ${ae_sdk}/Headers ${ae_sdk}/Headers/SP)
  # The SDK's headers pick their Windows types with _WINDOWS (Visual Studio's default).
  target_compile_definitions(MillSaphAE PRIVATE _WINDOWS)
  target_link_libraries(MillSaphAE PRIVATE millsaph_core)
  set_target_properties(MillSaphAE PROPERTIES
    OUTPUT_NAME MillSaph
    PREFIX ""
    SUFFIX ".aex"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/ae
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/ae  # Visual Studio
  )
  if(MILLSAPH_EDITOR) # the Lens Flare editor next to the .aex (Edit Lens)
    add_dependencies(MillSaphAE millsaph_lens_editor)
    add_custom_command(TARGET MillSaphAE POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy
                       $<TARGET_FILE:millsaph_lens_editor> ${CMAKE_BINARY_DIR}/ae/)
  endif()
endif()
