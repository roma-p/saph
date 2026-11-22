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
#include "effects/mask_input.h"
#include "effects/profile.h"
#include "engine/engine.h"
#include "halide/ms_limits.h"

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
    INVERT_MASK,
    SHOW_BLUR_AMOUNT1,
    SHOW_BLUR_AMOUNT2,
};
static const MsParam params[] = {
    MS_WIDTH(BLUR1, "blur_amount1", "Blur Amount1",
             "The width for the first blur. Sets the low frequency cutoff.", 0.014, 0.25),
    MS_WIDTH(BLUR2, "blur_amount2", "Blur Amount2",
             "The width for the second blur. Sets the high frequency cutoff.", 0.028, 0.25),
    MS_RELATIVE(REL_X, "blur_rel_x", "Blur Rel X", "Relative horizontal blur width.", 1.0),
    MS_RELATIVE(REL_Y, "blur_rel_y", "Blur Rel Y", "Relative vertical blur width.", 1.0),
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
    [BLUR_MASK] = MS_PARAM_BLUR_MASK(0.00625, 0.5),
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
    MS_SHOW(SHOW_BLUR_AMOUNT1, "ShowBlurAmount1", "Show Blur Amount1"),
    MS_SHOW(SHOW_BLUR_AMOUNT2, "ShowBlurAmount2", "Show Blur Amount2"),
};

// One render: images and settings of the kernels (src/halide/bandpass_generator.cpp,
// mask_generator.cpp).
typedef struct {
    MsImage *source, *output;
    MsMask *mask;
    int blur1_x, blur1_y, blur2_x, blur2_y; // total radii in pixels, rounded
    float brightness, saturation, offset_darks;
} Job;

// The mask value (with a Mask), then the band-pass, on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    const int status = ms_mask_run(j->mask, api);
    if (status != 0)
        return status;
    // The staged GPU version holds a box's windows in on-chip memory: up to box radii of
    // MS_STAGED_RADIUS_X along x and MS_STAGED_RADIUS_Y along y. The largest box of a
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
                                    box_x <= MS_STAGED_RADIUS_X && box_y <= MS_STAGED_RADIUS_Y
                                ? MS_K_BANDPASS_STAGED
                                : MS_K_BANDPASS;
    return MS_KERNEL(kernel, api, MS_IMAGE("source", j->source),
                     MS_IMAGE("mask_value", &j->mask->value), MS_INT("blur1_x", j->blur1_x),
                     MS_INT("blur1_y", j->blur1_y), MS_INT("blur2_x", j->blur2_x),
                     MS_INT("blur2_y", j->blur2_y), MS_FLOAT("brightness", j->brightness),
                     MS_FLOAT("saturation", j->saturation),
                     MS_FLOAT("offset_darks", j->offset_darks), MS_IMAGE("output", j->output));
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height;
    MsImage source, output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    const size_t floats[] = {ms_mask_floats(r, MASK)};
    float *block = malloc(ms_parts_size(floats, ARRAY_SIZE(floats))), *part[ARRAY_SIZE(floats)];
    if (!block)
        return ms_out_of_memory(r);
    ms_parts(block, floats, ARRAY_SIZE(floats), part);
    MsMask mask;
    ms_mask_init(&mask, r, MASK, SOURCE, &source, part[0], v[MASK_USE], v[INVERT_MASK],
                 v[BLUR_MASK]);
    // Total blur radii, rounded as the blur does (src/halide/blur.h).
    Job j = {&source,
             &output,
             &mask,
             (int)floor(v[BLUR1] * v[REL_X] + 0.5),
             (int)floor(v[BLUR1] * v[REL_Y] + 0.5),
             (int)floor(v[BLUR2] * v[REL_X] + 0.5),
             (int)floor(v[BLUR2] * v[REL_Y] + 0.5),
             (float)v[BRIGHTNESS],
             (float)v[SATURATION],
             (float)v[OFFSET_DARKS]};
    // Inputs uploaded to the GPU: the Source, and the Mask or the one mask value.
    MsImage *const inputs[] = {&source, mask.input};
    MsImage *const between[] = {ms_mask_between(&mask)}; // made on the GPU from the Mask
    const int ok = ms_run(run, &j, inputs, ARRAY_SIZE(inputs), between, ARRAY_SIZE(between),
                          &output, r->info, r->err, r->err_size);
    free(block);
    return ok;
}

// The viewer: Blur Amount1 (red), Blur Amount2 (green) and Blur Rel X and Y (its arrows).
static const MsOverlay overlay[] = {
    MS_OVERLAY_SIZED(MS_OVERLAY_ELLIPSE, "ShowBlurAmount1", "blur_amount1", NULL, "blur_rel_x",
                     "blur_rel_y", 0.0, MS_OVERLAY_RED),
    MS_OVERLAY_SIZED(MS_OVERLAY_ELLIPSE, "ShowBlurAmount2", "blur_amount2", NULL, "blur_rel_x",
                     "blur_rel_y", 0.0, MS_OVERLAY_GREEN),
    MS_OVERLAY_ARROWS("ShowBlurAmount2", "blur_amount2", "blur_rel_x", "blur_rel_y", 0.0,
                      MS_OVERLAY_GREEN),
};

const MsEffect ms_bandpass_effect = {
    .id = MS_ID("BandPass"),
    .label = "MS_BandPass",
    .description =
        "Band-pass filter: the difference of two blurs, scaled and offset by a gray value.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    MS_OVERLAYS(overlay),
    .presets = "bandpass",
};
