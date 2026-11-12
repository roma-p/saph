// MS_Mosaic: the image as a grid of flat cells.
//   - cells: Pixel Frequency across the image's width, Pixel Rel Height x as tall, the grid
//     centred on the image and moved by Pixel Shift
//   - each cell takes the colour (alpha included) of the Source at its centre, the Source
//     first blurred by Smooth Colors (3 box blurs, mirrored borders, total radius rounded);
//     the Crop Input edges limit the part of the Source the cells read
//     (src/halide/mosaic_generator.cpp)
//   - mixed over the Source by the (blurred) Mask, sampled at the cells' centres too, and by
//     Mix With Source
// Shifts, Smooth Colors, Blur Mask and the crops are "normalised" values, so hosts show them
// in pixels.

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
    FREQUENCY,
    REL_HEIGHT,
    SHIFT_X,
    SHIFT_Y,
    SMOOTH,
    MIX,
    BLUR_MASK,
    INVERT_MASK,
    MASK_USE,
    CROP_LEFT,
    CROP_RIGHT,
    CROP_TOP,
    CROP_BOTTOM
};
static const MsParam params[] = {
    [FREQUENCY] = {.type = MS_PARAM_DOUBLE,
                   .name = "pixel_frequency",
                   .label = "Pixel Frequency",
                   .hint = "The number of cells across the image's width.",
                   .def = 40.0,
                   .min = 1.0,
                   .max = MS_UNBOUNDED,
                   .display_min = 4.0,
                   .display_max = 200.0},
    [REL_HEIGHT] = {.type = MS_PARAM_DOUBLE,
                    .name = "pixel_rel_height",
                    .label = "Pixel Rel Height",
                    .hint = "The height of the cells relative to their width.",
                    .def = 1.0,
                    .min = 0.01,
                    .max = MS_UNBOUNDED,
                    .display_min = 0.1,
                    .display_max = 10.0},
    [SHIFT_X] = {.type = MS_PARAM_WIDTH,
                 .name = "pixel_shift_x",
                 .label = "Pixel Shift X",
                 .hint = "Moves the grid of cells horizontally.",
                 .display_min = -1.0,
                 .display_max = 1.0},
    [SHIFT_Y] = {.type = MS_PARAM_HEIGHT,
                 .name = "pixel_shift_y",
                 .label = "Pixel Shift Y",
                 .hint = "Moves the grid of cells vertically.",
                 .display_min = -1.0,
                 .display_max = 1.0},
    [SMOOTH] = {.type = MS_PARAM_WIDTH,
                .name = "smooth_colors",
                .label = "Smooth Colors",
                .hint = "Blurs the Source by this amount before taking the cells' colours.",
                .min = 0.0,
                .max = MS_UNBOUNDED,
                .display_max = 1.0},
    [MIX] = {.type = MS_PARAM_DOUBLE,
             .name = "mix_with_source",
             .label = "Mix With Source",
             .hint = "Mixes the Source back over the result: 0 for the mosaic only.",
             .min = 0.0,
             .max = 1.0,
             .display_max = 1.0},
    [BLUR_MASK] = {.type = MS_PARAM_WIDTH,
                   .name = "blur_mask",
                   .label = "Blur Mask",
                   .hint = "Blurs the Mask input by this amount before using.",
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 1.0},
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
    [MASK_USE] = MS_PARAM_MASK_USE,
    [CROP_LEFT] = {.type = MS_PARAM_WIDTH,
                   .name = "crop_left",
                   .label = "Crop Left",
                   .hint = "Leaves out this much of the Source's left side from the cells.",
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 2.0},
    [CROP_RIGHT] = {.type = MS_PARAM_WIDTH,
                    .name = "crop_right",
                    .label = "Crop Right",
                    .hint = "Leaves out this much of the Source's right side from the cells.",
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 2.0},
    [CROP_TOP] = {.type = MS_PARAM_HEIGHT,
                  .name = "crop_top",
                  .label = "Crop Top",
                  .hint = "Leaves out this much of the Source's top from the cells.",
                  .min = 0.0,
                  .max = MS_UNBOUNDED,
                  .display_max = 2.0},
    [CROP_BOTTOM] = {.type = MS_PARAM_HEIGHT,
                     .name = "crop_bottom",
                     .label = "Crop Bottom",
                     .hint = "Leaves out this much of the Source's bottom from the cells.",
                     .min = 0.0,
                     .max = MS_UNBOUNDED,
                     .display_max = 2.0},
};

// One render: images and settings of the kernels (src/halide/mosaic_generator.cpp,
// blur_generator.cpp, mask_generator.cpp).
typedef struct {
    MsImage *source, *mask, *mask_value, *smoothed, *output;
    int smooth, blur_mask; // total radii in pixels, rounded
    float cell_w, cell_h, origin_x, origin_y, mix;
    int kept_x0, kept_x1, kept_y0, kept_y1; // pixels the cells read, included
    int mask_alpha, invert_mask, has_mask;
} Job;

// The mask value (with a Mask), the smoothed Source (with Smooth Colors), then the cells,
// on `api`.
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
    if (j->smooth > 0) {
        // The prefix version costs the same whatever the radius: for large blurs. The largest
        // box of a blur of total radius t is (t + 2) / 3.
        const MsProfile *p = ms_profile(api);
        const MsKernel kernel = ms_kernel_built(MS_K_BLUR_PREFIX, api) && p->blur_prefix &&
                                        (j->smooth + 2) / 3 >= p->blur_prefix
                                    ? MS_K_BLUR_PREFIX
                                    : MS_K_BLUR;
        const int status = MS_KERNEL(kernel, api, MS_IMAGE("input", j->source),
                                     MS_INT("radius_x", j->smooth), MS_INT("radius_y", j->smooth),
                                     MS_IMAGE("output", j->smoothed));
        if (status != 0)
            return status;
    }
    return MS_KERNEL(MS_K_MOSAIC, api, MS_IMAGE("source", j->source),
                     MS_IMAGE("sampled", j->smooth > 0 ? j->smoothed : j->source),
                     MS_IMAGE("mask_value", j->mask_value), MS_FLOAT("cell_w", j->cell_w),
                     MS_FLOAT("cell_h", j->cell_h), MS_FLOAT("origin_x", j->origin_x),
                     MS_FLOAT("origin_y", j->origin_y), MS_INT("kept_x0", j->kept_x0),
                     MS_INT("kept_x1", j->kept_x1), MS_INT("kept_y0", j->kept_y0),
                     MS_INT("kept_y1", j->kept_y1), MS_FLOAT("mix", j->mix),
                     MS_IMAGE("output", j->output));
}

// The pixels [*first, *last] of `n` left by crops `a` (low side) and `b` (high side),
// truncated to whole pixels; at least one.
static void kept(int n, double a, double b, int *first, int *last) {
    const double lo = floor(a), hi = n - 1 - floor(b);
    *first = lo > n - 1 ? n - 1 : (int)lo;
    *last = hi < *first ? *first : (int)hi;
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height, has_mask = r->connected[MASK];
    const int smooth = (int)floor(v[SMOOTH] + 0.5);
    const size_t pixels = (size_t)w * h;
    // The mask value, made from the Mask; without one, 1 for every pixel (strides 0). The
    // smoothed Source.
    float *mask_value = has_mask ? malloc(pixels * sizeof(float)) : NULL;
    float *smoothed = smooth > 0 ? malloc(pixels * 4 * sizeof(float)) : NULL;
    if ((has_mask && !mask_value) || (smooth > 0 && !smoothed)) {
        free(mask_value);
        free(smoothed);
        snprintf(r->err, r->err_size, "not enough memory");
        return 0;
    }
    const float no_mask = 1.0f;
    MsImage source, mask, value, smooth_image, output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&mask, r->inputs[MASK].pixels, r->inputs[MASK].row, r->half, w, h);
    if (has_mask)
        ms_image(&value, mask_value, w, w, h, 1);
    else
        ms_image_constant(&value, &no_mask, w, h);
    ms_image(&smooth_image, smoothed, (ptrdiff_t)w * 4, w, h, 4);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    // A Mask that is the Source's image in the host: the Source stands in (uploaded once).
    const int mask_is_source = has_mask && ms_same_input(&r->inputs[MASK], &r->inputs[SOURCE]);
    const double cell_w = w / v[FREQUENCY];
    Job j = {.source = &source,
             .mask = mask_is_source ? &source : &mask,
             .mask_value = &value,
             .smoothed = &smooth_image,
             .output = &output,
             .smooth = smooth,
             .blur_mask = (int)floor(v[BLUR_MASK] + 0.5),
             .cell_w = (float)cell_w,
             .cell_h = (float)(cell_w * v[REL_HEIGHT]),
             .origin_x = (float)(w * 0.5 + v[SHIFT_X]),
             .origin_y = (float)(h * 0.5 + v[SHIFT_Y]),
             .mix = (float)v[MIX],
             .mask_alpha = (int)v[MASK_USE],
             .invert_mask = (int)v[INVERT_MASK],
             .has_mask = has_mask};
    kept(w, v[CROP_LEFT], v[CROP_RIGHT], &j.kept_x0, &j.kept_x1);
    kept(h, v[CROP_BOTTOM], v[CROP_TOP], &j.kept_y0, &j.kept_y1);
    // Inputs uploaded to the GPU: the Source, and the Mask or the one mask value.
    MsImage *const inputs[] = {&source, has_mask ? j.mask : &value};
    MsImage *between[2]; // made on the GPU: the mask value, the smoothed Source
    int between_count = 0;
    if (has_mask)
        between[between_count++] = &value;
    if (smooth > 0)
        between[between_count++] = &smooth_image;
    const int ok = ms_run(run, &j, inputs, 2, between, between_count, &output, r->info, r->err,
                          r->err_size);
    free(mask_value);
    free(smoothed);
    return ok;
}

// Mix With Source at 1: the Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    return v[MIX] >= 1.0;
}

const MsEffect ms_mosaic_effect = {
    MS_ID("Mosaic"),
    "MS_Mosaic",
    "Turns the image into a grid of flat cells, each the colour of the image at its centre.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
    identity,
    0,
    NULL,
};
