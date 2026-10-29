// MS_Test: a diagnostic effect, simple gain shader to validate shader scaffolding.
// Its "GPU" line shows which graphics card and driver are used, or why the GPU failed.

#include "common/defines.h"
#include "common/effect.h"
#include "gpu/gpu.h"
#include "ms_test_glsl.h"

// Inputs: clip, label in the host, name in the shader, value where there is no
// image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", "source", 0.0f, 0, 0},
};

enum { GAIN, GPU_INFO };
static const MsParam params[] = {
    [GAIN] = {.type = MS_PARAM_DOUBLE,
              .name = "gain",
              .label = "Gain",
              .hint = "Output = Source x Gain",
              .def = 1.0,
              .min = 0.0,
              .max = 100.0,
              .display_min = 0.0,
              .display_max = 4.0},
    [GPU_INFO] = {.type = MS_PARAM_TEXT, .name = "gpu", .label = "GPU", .text = ms_gpu_info},
};

static int render(const MsRender *r) {
    const MsShaderValue shader_values[] = {{"gain", (float)r->values[GAIN]}};
    const MsPass passes[] = {{ms_test_glsl, NULL, NULL, 0, 0, 0}};
    return ms_gpu_run(passes, ARRAY_SIZE(passes), r->inputs, ARRAY_SIZE(clips), r->width, r->height,
                      shader_values, ARRAY_SIZE(shader_values), r->output, r->err, r->err_size);
}

const MsEffect ms_test_effect = {
    MS_ID("Test"),
    "MS_Test",
    "MillSaph diagnostic: multiplies the Source by Gain.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
};
