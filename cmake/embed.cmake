# Builds shader files into the plugin: each .glsl becomes a C header holding its
# text, regenerated whenever the shader changes.
#
#   embed_shader(MillSaph src/ms_test.glsl)   ->  #include "ms_test_glsl.h"  (ms_test_glsl[])

if(CMAKE_SCRIPT_MODE_FILE)
  # Run at build time: write the bytes of INPUT as a C array named NAME into OUTPUT.
  file(READ ${INPUT} hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  file(WRITE ${OUTPUT} "static const char ${NAME}[] = {${bytes}0x00};\n")
  return()
endif()

function(embed_shader target shader)
  get_filename_component(stem ${shader} NAME_WE)
  set(name ${stem}_glsl)
  set(header ${CMAKE_BINARY_DIR}/generated/${name}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/${shader} -DOUTPUT=${header} -DNAME=${name}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${shader} cmake/embed.cmake)
  target_sources(${target} PRIVATE ${header})
  target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endfunction()
