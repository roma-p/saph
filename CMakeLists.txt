cmake_minimum_required(VERSION 3.18)

# macOS: one binary for Apple Silicon and Intel. Ignored on other systems.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
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
include(cmake/embed.cmake)

# src/common: shared by every layer; src/effects: the effects; src/gpu: GPU runner and
# backends; src/ofx: the OFX adapter. Includes name the folder: "common/blur.h".
file(GLOB sources CONFIGURE_DEPENDS src/*/*.c)
list(FILTER sources EXCLUDE REGEX "/gl_(mac|linux)\\.c$")
add_library(MillSaph MODULE ${sources})

file(GLOB shaders CONFIGURE_DEPENDS src/glsl/*.glsl)
foreach(shader ${shaders})
  embed_shader(MillSaph ${shader})
endforeach()
target_include_directories(MillSaph PRIVATE src ${third_party}/openfx/include ${third_party}/khronos)

find_package(Threads REQUIRED)
target_link_libraries(MillSaph PRIVATE Threads::Threads)

if(APPLE)
  set(platform MacOS)
  target_sources(MillSaph PRIVATE src/gpu/gl_mac.c)
  target_link_libraries(MillSaph PRIVATE "-framework OpenGL")
  target_compile_definitions(MillSaph PRIVATE GL_SILENCE_DEPRECATION)
elseif(WIN32)
  set(platform Win64)
  message(FATAL_ERROR "Windows: the OpenGL context (src/gpu/gl_windows.c) is not written yet")
else()
  set(platform Linux-x86-64)
  target_sources(MillSaph PRIVATE src/gpu/gl_linux.c)
  target_link_libraries(MillSaph PRIVATE ${CMAKE_DL_LIBS})
endif()

if(NOT MSVC)
  target_compile_options(MillSaph PRIVATE -Wall -Wextra -fvisibility=hidden)
endif()

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
