// MS_Test: a diagnostic effect, output = Source x Gain (src/halide/test_generator.cpp). Its
// "Runs on" line shows the API the kernels run on (or why none works).

#include <stdio.h>

#include "common/defines.h"
#include "common/effect.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
};

typedef struct {
    MsImage *source, *output;
    float gain;
} Job;

static int run(MsApi api, void *user) {
    const Job *j = user;
    return MS_KERNEL(MS_K_GAIN, api, MS_IMAGE("source", j->source), MS_FLOAT("gain", j->gain),
                     MS_IMAGE("output", j->output));
}

// Runs the kernel on a small image (the kernels need 16 x 16 pixels): the API it runs on,
// or why it does not.
static const char *runs_on(void) {
    static char line[300];
    static float pixels[16 * 16 * 4] = {0.5f}, result[16 * 16 * 4];
    MsImage source, output;
    ms_image(&source, pixels, 64, 16, 16, 4);
    ms_image(&output, result, 64, 16, 16, 4);
    Job j = {&source, &output, 2.0f};
    MsImage *const inputs[] = {&source};
    char err[256] = "";
    if (ms_run(run, &j, inputs, 1, NULL, 0, &output, err, sizeof err) && result[0] == 1.0f)
        snprintf(line, sizeof line, "%s", ms_api_name(ms_api_used()));
    else
        snprintf(line, sizeof line, "failed: %s", err[0] ? err : "wrong result");
    return line;
}

enum { GAIN, RUNS_ON };
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
    [RUNS_ON] = {.type = MS_PARAM_TEXT, .name = "gpu", .label = "Runs on", .text = runs_on},
};

static int render(const MsRender *r) {
    MsImage source, output;
    ms_image_rgba(&source, r->inputs[0].pixels, r->inputs[0].row, r->half, r->width, r->height);
    ms_image_rgba(&output, r->output, r->output_row, r->half, r->width, r->height);
    Job j = {&source, &output, (float)r->values[GAIN]};
    MsImage *const inputs[] = {&source};
    return ms_run(run, &j, inputs, 1, NULL, 0, &output, r->err, r->err_size);
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
    NULL,
};
