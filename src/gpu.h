// Runs GLSL fragment shaders on the GPU, in the plugin's own OpenGL context.
//
// All OpenGL work happens on one private thread that owns the context, so the
// host's own OpenGL state is never touched. Calls are serialised.

#pragma once

#include <stddef.h>

typedef struct {
    const char* name;
    float value;
} MsUniform;

// Runs one shader pass over a width x height RGBA float image (rows bottom-up,
// no padding). The shader reads the input as `uniform sampler2D source` and
// writes `out vec4 color`; `uniforms` are set as floats.
// Returns 1 on success, or 0 with the reason in err.
int ms_gpu_run(const char* shader, const float* input, int width, int height,
               const MsUniform* uniforms, int uniform_count, float* output,
               char* err, size_t err_size);

// One line describing the GPU in use, e.g. "OpenGL 4.1 | Apple M1 | CGL",
// or why it could not be started.
const char* ms_gpu_info(void);

// Stops the GPU thread and releases the OpenGL context. Must be called before the
// plugin is unloaded (the thread runs plugin code). The GPU restarts on next use.
void ms_gpu_shutdown(void);

// Implemented per platform (gpu_mac.c, gpu_linux.c): create an offscreen
// OpenGL 4.1 core context, current on the calling thread, destroy it, and look up functions.
int ms_gl_create_context(char* err, size_t err_size);
void ms_gl_destroy_context(void);
void* ms_gl_proc(const char* name);
extern const char* ms_gl_api;
