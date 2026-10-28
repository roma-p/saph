cmake_minimum_required(VERSION 3.18)

# macOS: one binary for Apple Silicon and Intel. Ignored on other systems.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

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

include(cmake/deps.cmake)
include(cmake/embed.cmake)

file(GLOB sources CONFIGURE_DEPENDS src/*.c)
list(FILTER sources EXCLUDE REGEX "/gpu_(mac|linux)\\.c$")
add_library(MillSaph MODULE ${sources})

file(GLOB shaders CONFIGURE_DEPENDS src/glsl/*.glsl)
foreach(shader ${shaders})
  embed_shader(MillSaph ${shader})
endforeach()
target_include_directories(MillSaph PRIVATE ${third_party}/openfx/include ${third_party}/khronos)

find_package(Threads REQUIRED)
target_link_libraries(MillSaph PRIVATE Threads::Threads)

if(APPLE)
  set(platform MacOS)
  target_sources(MillSaph PRIVATE src/gpu_mac.c)
  target_link_libraries(MillSaph PRIVATE "-framework OpenGL")
  target_compile_definitions(MillSaph PRIVATE GL_SILENCE_DEPRECATION)
elseif(WIN32)
  set(platform Win64)
  message(FATAL_ERROR "Windows: the GPU part (src/gpu_windows.c) is not written yet")
else()
  set(platform Linux-x86-64)
  target_sources(MillSaph PRIVATE src/gpu_linux.c)
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
