// MS_BandPass: band-pass filter, the difference of two blurs:
//   - each blur is 3 box blurs (horizontal then vertical), mirrored borders; the total
//     radius in pixels is round(amount x project width), split into 3 near-equal radii
//   - D = blur1 - blur2, saturated around its luma (5/16, 8/16, 3/16)
//   - result = 2 x Brightness x D + Offset Darks x (1 - 2D), mixed over the Source
//     through the (blurred) Mask; alpha unchanged
// The amounts are "normalised X" values, so hosts show them in pixels.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/profile.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
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
               .max = MS_UNBOUNDED,
               .display_max = 0.25},
    [BLUR2] = {.type = MS_PARAM_WIDTH,
               .name = "blur_amount2",
               .label = "Blur Amount2",
               .hint = "The width for the second blur. Sets the high frequency cutoff.",
               .def = 0.028,
               .min = 0.0,
               .max = MS_UNBOUNDED,
               .display_max = 0.25},
    [REL_X] = {.type = MS_PARAM_DOUBLE,
               .name = "blur_rel_x",
               .label = "Blur Rel X",
               .hint = "Relative horizontal blur width.",
               .def = 1.0,
               .min = 0.0,
               .max = MS_UNBOUNDED,
               .display_max = 5.0},
    [REL_Y] = {.type = MS_PARAM_DOUBLE,
               .name = "blur_rel_y",
               .label = "Blur Rel Y",
               .hint = "Relative vertical blur width.",
               .def = 1.0,
               .min = 0.0,
               .max = MS_UNBOUNDED,
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
                   .max = MS_UNBOUNDED,
                   .display_max = 0.5},
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
};

// One render: images and settings of the kernels (src/halide/bandpass_generator.cpp,
// mask_generator.cpp).
typedef struct {
    MsImage *source, *mask, *mask_value, *output;
    int blur1_x, blur1_y, blur2_x, blur2_y, blur_mask; // total radii in pixels, rounded
    float brightness, saturation, offset_darks;
    int mask_alpha, invert_mask, has_mask;
} Job;

// The mask value (with a Mask), then the band-pass, on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    if (j->has_mask) {
        const int status =
            MS_KERNEL(MS_K_MASK, api, MS_IMAGE("mask", j->mask),
                      MS_INT("mask_alpha", j->mask_alpha), MS_INT("invert_mask", j->invert_mask),
                      MS_INT("blur_mask", j->blur_mask), MS_IMAGE("mask_value", j->mask_value));
        if (status != 0)
            return status;
    }
    // The staged GPU version holds a box's windows in on-chip memory: up to box radii of 200
    // along x and 100 along y (src/halide/blur.h: STAGED_RADIUS_X / _Y). The largest box of a
    // blur of total radius t is (t + 2) / 3. The prefix version costs the same whatever the
    // radius: for large blurs.
    const int box_x = ((j->blur1_x > j->blur2_x ? j->blur1_x : j->blur2_x) + 2) / 3;
    const int box_y = ((j->blur1_y > j->blur2_y ? j->blur1_y : j->blur2_y) + 2) / 3;
    const MsProfile *p = ms_profile(api);
    const int box = box_x > box_y ? box_x : box_y;
    const MsKernel kernel = ms_kernel_built(MS_K_BANDPASS_PREFIX, api) && p->bandpass_prefix &&
                                    box >= p->bandpass_prefix
                                ? MS_K_BANDPASS_PREFIX
                            : ms_kernel_built(MS_K_BANDPASS_STAGED, api) && p->bandpass_staged &&
                                    box_x <= 200 && box_y <= 100
                                ? MS_K_BANDPASS_STAGED
                                : MS_K_BANDPASS;
    return MS_KERNEL(kernel, api, MS_IMAGE("source", j->source),
                     MS_IMAGE("mask_value", j->mask_value), MS_INT("blur1_x", j->blur1_x),
                     MS_INT("blur1_y", j->blur1_y), MS_INT("blur2_x", j->blur2_x),
                     MS_INT("blur2_y", j->blur2_y), MS_FLOAT("brightness", j->brightness),
                     MS_FLOAT("saturation", j->saturation),
                     MS_FLOAT("offset_darks", j->offset_darks), MS_IMAGE("output", j->output));
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height, has_mask = r->connected[MASK];
    // The mask value, made from the Mask; without one, 1 for every pixel (strides 0).
    float *mask_value = has_mask ? malloc((size_t)w * h * sizeof(float)) : NULL;
    if (has_mask && !mask_value) {
        snprintf(r->err, r->err_size, "not enough memory");
        return 0;
    }
    const float no_mask = 1.0f;
    MsImage source, mask, value, output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&mask, r->inputs[MASK].pixels, r->inputs[MASK].row, r->half, w, h);
    if (has_mask)
        ms_image(&value, mask_value, w, w, h, 1);
    else
        ms_image_constant(&value, &no_mask, w, h);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    // Total blur radii, rounded as the blur does (src/halide/blur.h).
    // A Mask that is the Source's image in the host: the Source stands in (uploaded once).
    const int mask_is_source = has_mask && ms_same_input(&r->inputs[MASK], &r->inputs[SOURCE]);
    Job j = {&source,
             mask_is_source ? &source : &mask,
             &value,
             &output,
             (int)floor(v[BLUR1] * v[REL_X] + 0.5),
             (int)floor(v[BLUR1] * v[REL_Y] + 0.5),
             (int)floor(v[BLUR2] * v[REL_X] + 0.5),
             (int)floor(v[BLUR2] * v[REL_Y] + 0.5),
             (int)floor(v[BLUR_MASK] + 0.5),
             (float)v[BRIGHTNESS],
             (float)v[SATURATION],
             (float)v[OFFSET_DARKS],
             (int)v[MASK_USE],
             (int)v[INVERT_MASK],
             has_mask};
    // Inputs uploaded to the GPU: the Source, and the Mask or the one mask value.
    MsImage *const inputs[] = {&source, has_mask ? &mask : &value};
    MsImage *const between[] = {&value}; // made on the GPU from the Mask
    const int ok = ms_run(run, &j, inputs, mask_is_source ? 1 : 2, between, has_mask, &output,
                          r->err, r->err_size);
    free(mask_value);
    return ok;
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
    NULL,
};
