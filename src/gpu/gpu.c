// GPU runner front: checks a job and hands it to the backend. See gpu.h.

#include <stdio.h>
#include <string.h>

#include "gpu/gpu.h"

static const MsGpuBackend *backend = &ms_gpu_opengl; // the only one so far

int ms_gpu_run(const MsPass *passes, int pass_count, const MsInput *inputs, int input_count,
               int width, int height, const MsShaderValue *values, int value_count, float *output,
               char *err, size_t err_size) {
    const int texture_count = input_count + pass_count; // inputs, then one result per pass
    if (pass_count < 1 || texture_count > MS_GPU_MAX_TEXTURES) {
        snprintf(err, err_size, "%d inputs and %d passes: too many", input_count, pass_count);
        return 0;
    }

    // Texture names: the inputs' own, then "pass1", "pass2"...
    const char *names[MS_GPU_MAX_TEXTURES];
    char pass_names[MS_GPU_MAX_TEXTURES][16];
    for (int i = 0; i < input_count; ++i)
        names[i] = inputs[i].name;
    for (int p = 0; p < pass_count; ++p) {
        snprintf(pass_names[p], sizeof pass_names[p], "pass%d", p + 1);
        names[input_count + p] = pass_names[p];
    }

    // Size of each result, and the texture each pass reads as `previous` (an input or
    // an earlier result).
    int widths[MS_GPU_MAX_TEXTURES], heights[MS_GPU_MAX_TEXTURES], previous[MS_GPU_MAX_TEXTURES];
    for (int p = 0; p < pass_count; ++p) {
        widths[p] = passes[p].width > 0 ? passes[p].width : width;
        heights[p] = passes[p].height > 0 ? passes[p].height : height;
        previous[p] = -1;
        if (!passes[p].previous) {
            previous[p] = p == 0 ? 0 : input_count + p - 1;
        } else {
            for (int i = 0; i < input_count + p; ++i)
                if (strcmp(names[i], passes[p].previous) == 0)
                    previous[p] = i;
        }
        if (previous[p] < 0) {
            snprintf(err, err_size, "pass %d: unknown previous \"%s\"", p + 1, passes[p].previous);
            return 0;
        }
    }
    if (widths[pass_count - 1] != width || heights[pass_count - 1] != height) {
        snprintf(err, err_size, "the last pass must make an image of the job's size");
        return 0;
    }

    const MsGpuJob job = {passes, pass_count, inputs,      input_count, width,
                          height, values,     value_count, names,       previous,
                          widths, heights,    output,      err,         err_size};
    return backend->run(&job);
}

const char *ms_gpu_info(void) { return backend->info(); }

void ms_gpu_shutdown(void) { backend->shutdown(); }
