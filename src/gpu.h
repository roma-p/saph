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

// Runs `pass_count` shaders in order over a width x height RGBA float image (packed,
// bottom row first), and writes the result of the last one into `output`. Each shader
// writes `out vec4 color` and can read the input as `source`, and the results of the
// passes before it as "pass1", "pass2", ... Intermediate results stay on the GPU.
// `uniforms` are set as floats in every pass (passes simply ignore the ones they do
// not declare). Returns 1 on success, or 0 with the reason in err.
int ms_gpu_run(const char* const* shaders, int pass_count, const float* input,
               int width, int height, const MsUniform* uniforms, int uniform_count,
               float* output, char* err, size_t err_size);

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
