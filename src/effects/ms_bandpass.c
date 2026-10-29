// MS_BandPass: band-pass filter, the difference of two blurs:
//   - each blur is 3 box blurs (horizontal then vertical), mirrored borders; the total
//     radius in pixels is round(amount x project width), split into 3 near-equal radii
//   - D = blur1 - blur2, saturated around its luma (5/16, 8/16, 3/16)
//   - result = 2 x Brightness x D + Offset Darks x (1 - 2D), mixed over the Source
//     through the (blurred) Mask; alpha unchanged
// The amounts are "normalised X" values, so hosts show them in pixels.

#include <math.h>

#include "bandpass_glsl.h"
#include "common/blur.h"
#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "gpu/gpu.h"
#include "mask_glsl.h"

// Inputs: clip, label in the host, name in the shader, value where there is no
// image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", "source", 0.0f, 0, 0},
    {"Mask", "Mask", "mask", 0.0f, 1, 1},
};
enum { SOURCE, MASK };

enum {
    BLUR1,
    BLUR2,
    REL_X,
    REL_Y,
    BRIGHTNESS,
    SATURATION,
    OFFSET_DARKS,
    MASK_USE,
    BLUR_MASK,
    INVERT_MASK
};
static const MsParam params[] = {
    [BLUR1] = {.type = MS_PARAM_WIDTH,
               .name = "blur_amount1",
               .label = "Blur Amount1",
               .hint = "The width for the first blur. Sets the low frequency cutoff.",
               .def = 0.014,
               .min = 0.0,
               .max = INFINITY,
               .display_max = 0.25},
    [BLUR2] = {.type = MS_PARAM_WIDTH,
               .name = "blur_amount2",
               .label = "Blur Amount2",
               .hint = "The width for the second blur. Sets the high frequency cutoff.",
               .def = 0.028,
               .min = 0.0,
               .max = INFINITY,
               .display_max = 0.25},
    [REL_X] = {.type = MS_PARAM_DOUBLE,
               .name = "blur_rel_x",
               .label = "Blur Rel X",
               .hint = "Relative horizontal blur width.",
               .def = 1.0,
               .min = 0.0,
               .max = INFINITY,
               .display_max = 5.0},
    [REL_Y] = {.type = MS_PARAM_DOUBLE,
               .name = "blur_rel_y",
               .label = "Blur Rel Y",
               .hint = "Relative vertical blur width.",
               .def = 1.0,
               .min = 0.0,
               .max = INFINITY,
               .display_max = 5.0},
    [BRIGHTNESS] = {.type = MS_PARAM_DOUBLE,
                    .name = "brightness",
                    .label = "Brightness",
                    .hint = "Scales the brightness of the result.",
                    .def = 3.0,
                    .display_min = -50.0,
                    .display_max = 50.0},
    [SATURATION] = {.type = MS_PARAM_DOUBLE,
                    .name = "saturation",
                    .label = "Saturation",
                    .hint = "Scales the color saturation. 0 for monochrome.",
                    .def = 1.0,
                    .display_max = 2.0},
    [OFFSET_DARKS] = {.type = MS_PARAM_DOUBLE,
                      .name = "offset_darks",
                      .label = "Offset Darks",
                      .hint = "Adds this gray value to the darker regions of the result.",
                      .def = 0.5,
                      .display_max = 1.0},
    [MASK_USE] = MS_PARAM_MASK_USE,
    [BLUR_MASK] = {.type = MS_PARAM_WIDTH,
                   .name = "blur_mask",
                   .label = "Blur Mask",
                   .hint = "Blurs the Mask input by this amount before using.",
                   .def = 0.00625,
                   .min = 0.0,
                   .max = INFINITY,
                   .display_max = 0.5},
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
};

static int render(const MsRender *r) {
    const double *v = r->values;
    const double blur1 = v[BLUR1], blur2 = v[BLUR2];
    const double blur_mask = r->connected[MASK] ? v[BLUR_MASK] : 0.0;

    // passes 1-6: blur 1 of the source, 7-12: blur 2 of the source,
    // 13: mask value, 14-19: blur of the mask, 20: result (see bandpass.glsl)
    MsPass passes[20];
    MsShaderValue radius[18];
    int n = ms_add_blur(passes, 0, radius, "source", blur1 * v[REL_X], blur1 * v[REL_Y]);
    n = ms_add_blur(passes, n, radius + 6, "source", blur2 * v[REL_X], blur2 * v[REL_Y]);
    passes[n++] = (MsPass){mask_glsl, NULL, NULL, 0, 0, 0};
    n = ms_add_blur(passes, n, radius + 12, NULL, blur_mask, blur_mask);
    passes[n++] = (MsPass){bandpass_glsl, NULL, NULL, 0, 0, 0};

    const MsShaderValue shader_values[] = {
        {"brightness", (float)v[BRIGHTNESS]},     {"saturation", (float)v[SATURATION]},
        {"offset_darks", (float)v[OFFSET_DARKS]}, {"mask_alpha", (float)v[MASK_USE]},
        {"invert_mask", (float)v[INVERT_MASK]},   {"has_mask", (float)r->connected[MASK]},
    };
    return ms_gpu_run(passes, n, r->inputs, ARRAY_SIZE(clips), r->width, r->height, shader_values,
                      ARRAY_SIZE(shader_values), r->output, r->err, r->err_size);
}

const MsEffect ms_bandpass_effect = {
    MS_ID("BandPass"),
    "MS_BandPass",
    "Band-pass filter: the difference of two blurs, scaled and offset by a gray value.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
};
