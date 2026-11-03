cmake_minimum_required(VERSION 3.18)

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
# the OFX plugin, src/ae the After Effects plugin (Windows). Includes name the folder:
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
  target_link_libraries(millsaph_core PUBLIC ${CMAKE_DL_LIBS})
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
add_executable(millsaph_bench tools/perf/bench.c tools/perf/images.c tools/perf/transfer.c)
target_link_libraries(millsaph_bench PRIVATE millsaph_core)
if(NOT WIN32)
  target_link_libraries(millsaph_bench PRIVATE m)
endif()
millsaph_warnings(millsaph_bench)

# ---------------------------------------------------------------- After Effects plugin

# Windows only for now. Needs the After Effects SDK, which Adobe distributes to Adobe
# accounts (developer.adobe.com): unzip it so that third_party/AfterEffectsSDK/Examples
# exists. Without it, only the OFX plugin is built.
set(ae_sdk ${third_party}/AfterEffectsSDK/Examples)
if(WIN32 AND MSVC AND EXISTS ${ae_sdk}/Headers/AE_Effect.h)
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
  target_link_libraries(MillSaphAE PRIVATE millsaph_core)
  set_target_properties(MillSaphAE PROPERTIES
    OUTPUT_NAME MillSaph
    PREFIX ""
    SUFFIX ".aex"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/ae
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/ae  # Visual Studio
  )
endif()
