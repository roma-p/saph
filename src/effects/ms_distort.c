// MS_Distort: warps the Source by the slopes of a Lens image. In Halide
// (src/halide/distort_generator.cpp), on the GPU when possible, on the CPU with Enable GPU
// off:
//   - lens = luma (5/16, 8/16, 3/16) of the Lens input (the Source when it is not
//     connected), blurred by Blur Lens (3-box blur) and then scaled by 1 + 80 x Blur Lens
//     (a fraction of the frame's width)
//   - shift = Amount x 0.005 x W x W x slope of the lens (central difference), W the
//     project width in pixels; Fine divides by 100; Amount Rel X/Y scale each axis,
//     then Rotate Warp Dir turns it counterclockwise; rounded to 1/256 pixel
//   - output pixel p = Source at p + shift; Wrap X/Y: No (transparent), Tile, Reflect,
//     inside the Source minus the Crop margins
//   - Filter: where the warp shrinks the image, the Source is read from box-averaged
//     reduced copies (levels), blending two levels: level = fast log2 of how fast the
//     shift changes per pixel, per axis (both axes reduced, or only one)
//   - Mask: Post-effect mixes Source and result by the mask; Pre-effect uses the mask
//     moved by the same shift; Mask Lens multiplies the lens by the mask

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/input.h"
#include "effects/mask_input.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Lens", "Lens", 0.0f, 1, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
};
enum { SOURCE, LENS, MASK };

enum { WRAP_NO, WRAP_TILE, WRAP_REFLECT };

static const char *const apply[] = {"Post-effect", "Pre-effect"};
static const char *const wraps[] = {"No", "Tile", "Reflect"};
enum {
    APPLY_MASK,
    MASK_LENS,
    AMOUNT,
    FINE,
    BLUR_LENS,
    ROTATE,
    REL_X,
    REL_Y,
    WRAP_X,
    WRAP_Y,
    FILTER,
    BLUR_MASK,
    INVERT_MASK,
    MASK_USE,
    CROP_LEFT,
    CROP_RIGHT,
    CROP_TOP,
    CROP_BOTTOM,
    ENABLE_GPU,
};
static const MsParam params[] = {
    [APPLY_MASK] =
        {.type = MS_PARAM_CHOICE,
         .name = "apply_mask",
         .label = "Apply Mask",
         .hint =
             "Post-effect mixes the result with the Source by the Mask; Pre-effect moves the Mask "
             "with the image.",
         .entries = apply,
         .entry_count = 2},
    [MASK_LENS] = {.type = MS_PARAM_BOOL,
                   .name = "mask_lens",
                   .label = "Mask Lens",
                   .hint = "Multiply the Lens by the Mask."},
    [AMOUNT] = {.type = MS_PARAM_DOUBLE,
                .name = "amount",
                .label = "Amount",
                .hint = "Scales the amount of distortion.",
                .def = 1.0,
                .display_min = -2.0,
                .display_max = 2.0},
    [FINE] = {.type = MS_PARAM_BOOL,
              .name = "fine",
              .label = "Fine",
              .hint = "Divides the Amount by 100, for subtle distortions."},
    MS_WIDTH(BLUR_LENS, "blur_lens", "Blur Lens", "Blurs the Lens before computing its slopes.",
             0.05, 0.25),
    [ROTATE] = {.type = MS_PARAM_ANGLE,
                .name = "rotate_warp_dir",
                .label = "Rotate Warp Dir",
                .hint = "Rotates the direction of the displacement.",
                .display_min = -360.0,
                .display_max = 360.0},
    MS_RELATIVE(REL_X, "amount_rel_x", "Amount Rel X", "Relative horizontal distortion.", 1.0),
    MS_RELATIVE(REL_Y, "amount_rel_y", "Amount Rel Y", "Relative vertical distortion.", 1.0),
    [WRAP_X] = {.type = MS_PARAM_CHOICE,
                .name = "wrap_x",
                .label = "Wrap X",
                .hint = "What is shown when the warp reads beyond the left or right edge.",
                .def = WRAP_REFLECT,
                .entries = wraps,
                .entry_count = 3},
    [WRAP_Y] = {.type = MS_PARAM_CHOICE,
                .name = "wrap_y",
                .label = "Wrap Y",
                .hint = "What is shown when the warp reads beyond the top or bottom edge.",
                .def = WRAP_REFLECT,
                .entries = wraps,
                .entry_count = 3},
    [FILTER] = {.type = MS_PARAM_BOOL,
                .name = "filter",
                .label = "Filter",
                .hint = "Smooths the Source where the warp shrinks it, to avoid aliasing.",
                .def = 1},
    MS_WIDTH(BLUR_MASK, "blur_mask", "Blur Mask", "Blurs the Mask.", 0.0, 0.25),
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Apply the effect where the Mask is black."),
    [MASK_USE] = MS_PARAM_MASK_USE,
    [CROP_LEFT] = {.type = MS_PARAM_WIDTH,
                   .name = "crop_left",
                   .label = "Crop Left",
                   .hint = "Ignore this part of the Source on the left.",
                   .min = 0.0,
                   .max = 1.0,
                   .display_max = 1.0},
    [CROP_RIGHT] = {.type = MS_PARAM_WIDTH,
                    .name = "crop_right",
                    .label = "Crop Right",
                    .hint = "Ignore this part of the Source on the right.",
                    .min = 0.0,
                    .max = 1.0,
                    .display_max = 1.0},
    [CROP_TOP] = {.type = MS_PARAM_HEIGHT,
                  .name = "crop_top",
                  .label = "Crop Top",
                  .hint = "Ignore this part of the Source at the top.",
                  .min = 0.0,
                  .max = 1.0,
                  .display_max = 1.0},
    [CROP_BOTTOM] = {.type = MS_PARAM_HEIGHT,
                     .name = "crop_bottom",
                     .label = "Crop Bottom",
                     .hint = "Ignore this part of the Source at the bottom.",
                     .min = 0.0,
                     .max = 1.0,
                     .display_max = 1.0},
    [ENABLE_GPU] = {.type = MS_PARAM_BOOL,
                    .name = "enable_gpu",
                    .label = "Enable GPU",
                    .hint = "Render on the GPU; off renders on the CPU (same result).",
                    .def = 1},
};

#define MAX_LEVELS 16 // reduced copies per axis, at most (distort_generator.cpp)

// ---------------------------------------------------------------- Halide

// Number of reduced copies along an axis of n pixels (down to 1 pixel).
static int max_levels(int n) {
    int levels = 1;
    while (levels < MAX_LEVELS && (n >> levels) >= 1)
        ++levels;
    return levels;
}

// Size along an axis of `length` pixels of the image holding levels 0 to count - 1.
static int levels_size(int length, int count) {
    int size = 0;
    for (int n = 0; n < count; ++n)
        size += length >> n;
    return size;
}

// One render: the images and settings of the kernels (src/halide/distort_generator.cpp,
// mask_generator.cpp).
typedef struct {
    MsImage *source, *lens, *output; // lens: the Source if none
    MsMask *mask;
    MsImage *shift, *largest; // between the kernels
    MsImage *levels;          // Filter: sized by run()
    int width, height;
    double amount; // shift in pixels per unit of lens slope (per pixel)
    double rel_x, rel_y;
    double cos_r, sin_r;                   // Rotate Warp Dir
    double blur_lens;                      // pixels
    double lens_gain;                      // the blurred lens' slope x this
    double crop_l, crop_r, crop_b, crop_t; // pixels
    int wrap_x, wrap_y;
    int filter;
    int mask_lens, pre_effect;
    int gpu;
} Job;

static int run_shift(MsApi api, const Job *j) {
    return MS_KERNEL(
        MS_K_DISTORT_SHIFT, api, MS_IMAGE("lens", j->lens), MS_IMAGE("mask_value", &j->mask->value),
        MS_INT("mask_lens", j->mask->has_mask && j->mask_lens),
        MS_INT("blur_lens", floor(j->blur_lens + 0.5)),
        MS_FLOAT("amount", j->amount * j->lens_gain), MS_FLOAT("rel_x", j->rel_x),
        MS_FLOAT("rel_y", j->rel_y), MS_FLOAT("cos_r", j->cos_r), MS_FLOAT("sin_r", j->sin_r),
        MS_IMAGE("shift", j->shift), MS_IMAGE("largest_lod", j->largest));
}

static int run_levels(MsApi api, const Job *j, int levels_x, int levels_y) {
    return MS_KERNEL(MS_K_DISTORT_LEVELS, api, MS_IMAGE("source", j->source),
                     MS_INT("levels_x", levels_x), MS_INT("levels_y", levels_y),
                     MS_IMAGE("output", j->levels));
}

static int run_warp(MsApi api, const Job *j, MsImage *levels, int levels_x, int levels_y) {
    return MS_KERNEL(
        MS_K_DISTORT_WARP, api, MS_IMAGE("source", j->source), MS_IMAGE("shift", j->shift),
        MS_IMAGE("mask_value", &j->mask->value), MS_IMAGE("levels", levels),
        MS_INT("filter", j->filter), MS_INT("levels_x", levels_x), MS_INT("levels_y", levels_y),
        MS_INT("wrap_x", j->wrap_x), MS_INT("wrap_y", j->wrap_y), MS_FLOAT("crop_l", j->crop_l),
        MS_FLOAT("crop_r", j->crop_r), MS_FLOAT("crop_b", j->crop_b), MS_FLOAT("crop_t", j->crop_t),
        MS_INT("has_mask", j->mask->has_mask), MS_INT("pre_effect", j->pre_effect),
        MS_IMAGE("output", j->output));
}

// The kernels on `api`: mask (with a Mask), shift, levels (Filter), warp. With Enable GPU
// off, only on the CPU.
static int run(MsApi api, void *user) {
    Job *j = user;
    if (!j->gpu && api != MS_API_CPU)
        return MS_RUN_DECLINE;
    int status = ms_mask_run(j->mask, api);
    if (status != 0)
        return status;
    status = run_shift(api, j);
    if (status != 0)
        return status;

    // Filter: the levels the largest level of detail needs, capped by the image size.
    MsImage *levels = j->source;
    int levels_x = 1, levels_y = 1;
    if (j->filter) {
        status = halide_copy_to_host(NULL, &j->largest->b);
        if (status != 0)
            return status;
        const float *largest = (const float *)j->largest->b.host;
        levels_x = (int)ceil(largest[0]) + 1;
        levels_y = (int)ceil(largest[1]) + 1;
        if (levels_x > max_levels(j->width))
            levels_x = max_levels(j->width);
        if (levels_y > max_levels(j->height))
            levels_y = max_levels(j->height);
        const int w = levels_size(j->width, levels_x), h = levels_size(j->height, levels_y);
        levels = j->levels;
        levels->dim[0].extent = w;
        levels->dim[1].extent = h;
        levels->dim[1].stride = w * 4;
        status = ms_image_device(levels, api); // now that its size is known
        if (status == 0)
            status = run_levels(api, j, levels_x, levels_y);
        if (status != 0)
            return status;
    }
    return run_warp(api, j, levels, levels_x, levels_y);
}

// ---------------------------------------------------------------- render

static int render(const MsRender *r) {
    const double *v = r->values;
    const int width = r->width, height = r->height;
    const double w = r->frame_width; // a width of 1.0 in pixels
    const double angle = v[ROTATE] * M_PI / 180.0;
    Job job = {.width = width,
               .height = height,
               // Shift = Amount x 0.005 x W x (lens slope per pixel x W).
               .amount = v[AMOUNT] * 0.005 * w * w,
               .rel_x = v[REL_X],
               .rel_y = v[REL_Y],
               .cos_r = cos(angle),
               .sin_r = sin(angle),
               .blur_lens = v[BLUR_LENS],
               .crop_l = v[CROP_LEFT],
               .crop_r = v[CROP_RIGHT],
               .crop_b = v[CROP_BOTTOM],
               .crop_t = v[CROP_TOP],
               .wrap_x = (int)v[WRAP_X],
               .wrap_y = (int)v[WRAP_Y],
               .filter = (int)v[FILTER],
               .mask_lens = (int)v[MASK_LENS],
               .pre_effect = v[APPLY_MASK] == 1,
               .gpu = (int)v[ENABLE_GPU]};
    if (v[FINE])
        job.amount /= 100.0;
    // a blurred lens' slopes x 1 + 80 Blur Lens (a fraction of the width): 1 + blur / 24 at
    // 1920 wide, 1 + blur / 6 at 480 (the reference's, exact on a ramp at 480 and 960)
    job.lens_gain = job.blur_lens > 0 ? 1.0 + 80.0 * job.blur_lens / fmax(w, 1.0) : 1.0;

    // Between the kernels: the shift (4 values per pixel), with the Filter the levels (at most
    // 4 times the image; only the part the levels need is written, the rest never touched),
    // the mask value.
    const size_t levels_floats = job.filter ? (size_t)levels_size(width, max_levels(width)) *
                                                  levels_size(height, max_levels(height)) * 4
                                            : 0;
    const size_t floats[] = {(size_t)width * height * 4, levels_floats, ms_mask_floats(r, MASK)};
    float *block = malloc(ms_parts_size(floats, ARRAY_SIZE(floats))), *part[ARRAY_SIZE(floats)];
    if (!block)
        return ms_out_of_memory(r);
    ms_parts(block, floats, ARRAY_SIZE(floats), part);
    float largest[2] = {0.0f, 0.0f};
    MsImage source, lens, output, shift, largest_image, levels;
    ms_input_image(&source, r, SOURCE);
    ms_output_image(&output, r);
    ms_image(&shift, part[0], (ptrdiff_t)width * 4, width, height, 4);
    ms_image(&largest_image, largest, 2, 2, 1, 1);
    largest_image.b.dimensions = 1;
    ms_image(&levels, part[1], 4, 1, 1, 4); // sized by run()
    MsMask mask;
    ms_mask_init(&mask, r, MASK, SOURCE, &source, part[2], v[MASK_USE], v[INVERT_MASK],
                 v[BLUR_MASK]);
    job.source = &source;
    // Without a Lens, the Source stands in (only read without a Lens); a Lens that is the
    // Source's image itself (the host gives the same pixels): the Source, uploaded once.
    job.lens = ms_input_or(&lens, r, LENS, r->connected[LENS], &source, SOURCE);
    job.mask = &mask;
    job.output = &output;
    job.shift = &shift;
    job.largest = &largest_image;
    job.levels = &levels;
    // Uploaded: the Source, the Lens and Mask standing in (ms_run uploads an image given
    // twice once); without a mask, its one value (with one, it is made there).
    MsImage *const inputs[] = {&source, job.lens, mask.input};
    MsImage *const between[] = {&shift, ms_mask_between(&mask), &largest_image,
                                job.filter ? &levels : NULL};
    const int ok = ms_run(run, &job, inputs, ARRAY_SIZE(inputs), between, ARRAY_SIZE(between),
                          &output, r->info, r->err, r->err_size);
    free(block);
    return ok;
}

const MsEffect ms_distort_effect = {
    .id = MS_ID("Distort"),
    .label = "MS_Distort",
    .description =
        "Warps the Source by the slopes of the Lens input, like looking through textured glass.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    .presets = "distort",
};
