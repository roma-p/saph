// Runs GLSL fragment shaders on the GPU. The passes are described here without any GPU
// API; gpu.c checks them and hands them to a backend, which does the work: OpenGL
// (gl.c) for now.

#pragma once

#include <stddef.h>

// A value handed to the shaders of a pass: sets the shader's `uniform float <name>`
// (GLSL). Checkboxes and menus are sent as 0, 1, 2... A shader that does not declare
// the name ignores it.
//
// A value handed to the shaders, true for every pixel (glsl "uniform")
typedef struct {
    const char *name;
    float value;
} MsShaderValue;

// An image the shaders can read, as `uniform sampler2D <name>`: width x height
// RGBA floats, packed, bottom row first.
typedef struct {
    const char *name;
    const float *pixels;
} MsInput;

// One shader pass.
typedef struct {
    const char *shader;
    const char *previous;        // texture read as `previous`: an input or "passN" name;
                                 // NULL = the pass just before (the first input for pass 1)
    const MsShaderValue *values; // set for this pass only, after the shared ones (may be NULL)
    int value_count;
    int width, height; // size of this pass's result; 0 = the size of the job
                       // (the last pass must be the size of the job)
} MsPass;

// Runs `pass_count` passes in order over width x height RGBA float images (a pass can
// make a result of another size, e.g. a reduced copy), and writes the result of the
// last one into `output`. Each shader writes `out vec4 color` and
// can read every input by its name, the results of earlier passes as "pass1",
// "pass2"..., and one chosen texture as `previous`. Intermediate results stay on the
// GPU and are freed after the last pass that reads them. The shared `values` are
// set as floats in every pass (passes ignore the ones they do not declare).
// Returns 1 on success, or 0 with the reason in err.
int ms_gpu_run(const MsPass *passes, int pass_count, const MsInput *inputs, int input_count,
               int width, int height, const MsShaderValue *values, int value_count, float *output,
               char *err, size_t err_size);

// One line describing the GPU in use, e.g. "OpenGL 4.1 | Apple M1 | CGL",
// or why it could not be started.
const char *ms_gpu_info(void);

// Releases the GPU. Must be called before the plugin is unloaded (the backend may run
// a thread with plugin code). The GPU restarts on next use.
void ms_gpu_shutdown(void);

// ---------------------------------------------------------------- backends

#define MS_GPU_MAX_TEXTURES 64 // inputs + pass results in one job

// A job checked by ms_gpu_run. Textures are numbered inputs first, then one result per
// pass (texture input_count + p is the result of pass p).
typedef struct {
    const MsPass *passes;
    int pass_count;
    const MsInput *inputs;
    int input_count;
    int width, height;
    const MsShaderValue *values; // shared by every pass
    int value_count;
    const char *const *names; // name of each texture in the shaders
    const int *previous;      // texture each pass reads as `previous`
    const int *pass_width;    // size of each pass's result
    const int *pass_height;
    float *output;
    char *err;
    size_t err_size;
} MsGpuJob;

typedef struct {
    const char *(*info)(void);       // see ms_gpu_info (starts the GPU on first use)
    int (*run)(const MsGpuJob *job); // 1 on success, or 0 with the reason in job->err
    void (*shutdown)(void);          // see ms_gpu_shutdown
} MsGpuBackend;

extern const MsGpuBackend ms_gpu_opengl; // gl.c
