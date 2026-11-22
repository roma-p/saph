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
#include "effects/input.h"
#include "effects/mask_input.h"
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
    MS_WIDTH(SMOOTH, "smooth_colors", "Smooth Colors",
             "Blurs the Source by this amount before taking the cells' colours.", 0.0, 1.0),
    MS_FRACTION(MIX, "mix_with_source", "Mix With Source",
                "Mixes the Source back over the result: 0 for the mosaic only.", 0.0),
    [BLUR_MASK] = MS_PARAM_BLUR_MASK(0.0, 1.0),
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
    [MASK_USE] = MS_PARAM_MASK_USE,
    MS_WIDTH(CROP_LEFT, "crop_left", "Crop Left",
             "Leaves out this much of the Source's left side from the cells.", 0.0, 2.0),
    MS_WIDTH(CROP_RIGHT, "crop_right", "Crop Right",
             "Leaves out this much of the Source's right side from the cells.", 0.0, 2.0),
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
    MsImage *source, *smoothed, *output;
    MsMask *mask;
    int smooth; // total radius in pixels, rounded
    float cell_w, cell_h, origin_x, origin_y, mix;
    int kept_x0, kept_x1, kept_y0, kept_y1; // pixels the cells read, included
} Job;

// The mask value (with a Mask), the smoothed Source (with Smooth Colors), then the cells,
// on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    int status = ms_mask_run(j->mask, api);
    if (status == 0 && j->smooth > 0)
        status = ms_rgba_blur(api, j->source, j->smooth, j->smooth, j->smoothed);
    if (status != 0)
        return status;
    return MS_KERNEL(MS_K_MOSAIC, api, MS_IMAGE("source", j->source),
                     MS_IMAGE("sampled", j->smooth > 0 ? j->smoothed : j->source),
                     MS_IMAGE("mask_value", &j->mask->value), MS_FLOAT("cell_w", j->cell_w),
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
    const int w = r->width, h = r->height;
    const int smooth = (int)floor(v[SMOOTH] + 0.5);
    MsImage source, smooth_image, output;
    ms_input_image(&source, r, SOURCE);
    ms_output_image(&output, r);
    // The mask value and the smoothed Source.
    const size_t floats[] = {ms_mask_floats(r, MASK), smooth > 0 ? (size_t)w * h * 4 : 0};
    float *block = malloc(ms_parts_size(floats, ARRAY_SIZE(floats))), *part[ARRAY_SIZE(floats)];
    if (!block)
        return ms_out_of_memory(r);
    ms_parts(block, floats, ARRAY_SIZE(floats), part);
    MsMask mask;
    ms_mask_init(&mask, r, MASK, SOURCE, &source, part[0], v[MASK_USE], v[INVERT_MASK],
                 v[BLUR_MASK]);
    ms_image(&smooth_image, part[1], (ptrdiff_t)w * 4, w, h, 4);
    const double cell_w = w / v[FREQUENCY];
    Job j = {.source = &source,
             .smoothed = &smooth_image,
             .output = &output,
             .mask = &mask,
             .smooth = smooth,
             .cell_w = (float)cell_w,
             .cell_h = (float)(cell_w * v[REL_HEIGHT]),
             .origin_x = (float)(w * 0.5 + v[SHIFT_X]),
             .origin_y = (float)(h * 0.5 + v[SHIFT_Y]),
             .mix = (float)v[MIX]};
    kept(w, v[CROP_LEFT], v[CROP_RIGHT], &j.kept_x0, &j.kept_x1);
    kept(h, v[CROP_BOTTOM], v[CROP_TOP], &j.kept_y0, &j.kept_y1);
    // Inputs uploaded to the GPU: the Source, and the Mask or the one mask value.
    MsImage *const inputs[] = {&source, mask.input};
    MsImage *const between[] = {ms_mask_between(&mask), smooth > 0 ? &smooth_image : NULL};
    const int ok = ms_run(run, &j, inputs, ARRAY_SIZE(inputs), between, ARRAY_SIZE(between),
                          &output, r->info, r->err, r->err_size);
    free(block);
    return ok;
}

// Mix With Source at 1: the Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    return v[MIX] >= 1.0;
}

const MsEffect ms_mosaic_effect = {
    .id = MS_ID("Mosaic"),
    .label = "MS_Mosaic",
    .description =
        "Turns the image into a grid of flat cells, each the colour of the image at its centre.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    .identity = identity,
    .presets = "mosaic",
};
