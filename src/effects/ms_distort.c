// MS_Distort: warps the Source by the slopes of a Lens image. In Halide
// (src/halide/distort_generator.cpp), on the GPU when possible, on the CPU with Enable GPU
// off:
//   - lens = luma (5/16, 8/16, 3/16) of the Lens input (the Source when it is not
//     connected), blurred by Blur Lens (3-box blur) and then scaled by 1 + blur / 24
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
    [BLUR_LENS] = {.type = MS_PARAM_WIDTH,
                   .name = "blur_lens",
                   .label = "Blur Lens",
                   .hint = "Blurs the Lens before computing its slopes.",
                   .def = 0.05,
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 0.25},
    [ROTATE] = {.type = MS_PARAM_ANGLE,
                .name = "rotate_warp_dir",
                .label = "Rotate Warp Dir",
                .hint = "Rotates the direction of the displacement.",
                .display_min = -360.0,
                .display_max = 360.0},
    [REL_X] = {.type = MS_PARAM_DOUBLE,
               .name = "amount_rel_x",
               .label = "Amount Rel X",
               .hint = "Relative horizontal distortion.",
               .def = 1.0,
               .min = 0.0,
               .max = MS_UNBOUNDED,
               .display_max = 5.0},
    [REL_Y] = {.type = MS_PARAM_DOUBLE,
               .name = "amount_rel_y",
               .label = "Amount Rel Y",
               .hint = "Relative vertical distortion.",
               .def = 1.0,
               .min = 0.0,
               .max = MS_UNBOUNDED,
               .display_max = 5.0},
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
    [BLUR_MASK] = {.type = MS_PARAM_WIDTH,
                   .name = "blur_mask",
                   .label = "Blur Mask",
                   .hint = "Blurs the Mask.",
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 0.25},
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

typedef struct {
    double amount; // shift in pixels per unit of lens slope (per pixel)
    double rel_x, rel_y;
    double cos_r, sin_r;                   // Rotate Warp Dir
    double blur_lens, blur_mask;           // pixels
    double crop_l, crop_r, crop_b, crop_t; // pixels
    int wrap_x, wrap_y;
    int filter;
    int has_lens, has_mask, mask_alpha, invert_mask, mask_lens, pre_effect;
    int gpu;
} Settings;

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
    const Settings *s;
    MsImage *source, *lens, *mask, *output; // lens: the Source if none
    MsImage *shift, *mask_value, *largest;  // between the kernels
    MsImage *levels;                        // Filter: sized by run()
    int width, height;
} Job;

static int run_mask(MsApi api, const Job *j) {
    const Settings *s = j->s;
    return MS_KERNEL(MS_K_MASK, api, MS_IMAGE("mask", j->mask), MS_INT("mask_alpha", s->mask_alpha),
                     MS_INT("invert_mask", s->invert_mask),
                     MS_INT("blur_mask", floor(s->blur_mask + 0.5)),
                     MS_IMAGE("mask_value", j->mask_value));
}

static int run_shift(MsApi api, const Job *j) {
    const Settings *s = j->s;
    const double gain = s->blur_lens > 0 ? 1.0 + s->blur_lens / 24.0 : 1.0; // a blurred lens
    return MS_KERNEL(
        MS_K_DISTORT_SHIFT, api, MS_IMAGE("lens", j->lens), MS_IMAGE("mask_value", j->mask_value),
        MS_INT("mask_lens", s->has_mask && s->mask_lens),
        MS_INT("blur_lens", floor(s->blur_lens + 0.5)), MS_FLOAT("amount", s->amount * gain),
        MS_FLOAT("rel_x", s->rel_x), MS_FLOAT("rel_y", s->rel_y), MS_FLOAT("cos_r", s->cos_r),
        MS_FLOAT("sin_r", s->sin_r), MS_IMAGE("shift", j->shift),
        MS_IMAGE("largest_lod", j->largest));
}

static int run_levels(MsApi api, const Job *j, int levels_x, int levels_y) {
    return MS_KERNEL(MS_K_DISTORT_LEVELS, api, MS_IMAGE("source", j->source),
                     MS_INT("levels_x", levels_x), MS_INT("levels_y", levels_y),
                     MS_IMAGE("output", j->levels));
}

static int run_warp(MsApi api, const Job *j, MsImage *levels, int levels_x, int levels_y) {
    const Settings *s = j->s;
    return MS_KERNEL(
        MS_K_DISTORT_WARP, api, MS_IMAGE("source", j->source), MS_IMAGE("shift", j->shift),
        MS_IMAGE("mask_value", j->mask_value), MS_IMAGE("levels", levels),
        MS_INT("filter", s->filter), MS_INT("levels_x", levels_x), MS_INT("levels_y", levels_y),
        MS_INT("wrap_x", s->wrap_x), MS_INT("wrap_y", s->wrap_y), MS_FLOAT("crop_l", s->crop_l),
        MS_FLOAT("crop_r", s->crop_r), MS_FLOAT("crop_b", s->crop_b), MS_FLOAT("crop_t", s->crop_t),
        MS_INT("has_mask", s->has_mask), MS_INT("pre_effect", s->pre_effect),
        MS_IMAGE("output", j->output));
}

// The kernels on `api`: mask (with a Mask), shift, levels (Filter), warp. With Enable GPU
// off, only on the CPU.
static int run(MsApi api, void *user) {
    Job *j = user;
    if (!j->s->gpu && api != MS_API_CPU)
        return MS_RUN_DECLINE;
    int status = j->s->has_mask ? run_mask(api, j) : 0;
    if (status != 0)
        return status;
    status = run_shift(api, j);
    if (status != 0)
        return status;

    // Filter: the levels the largest level of detail needs, capped by the image size.
    MsImage *levels = j->source;
    int levels_x = 1, levels_y = 1;
    if (j->s->filter) {
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

static int process(const MsRender *r, const Settings *s) {
    const MsInput *inputs = r->inputs;
    const int half = r->half, width = r->width, height = r->height;
    const size_t pixels = (size_t)width * height;
    // Between the kernels: the shift (4 values per pixel), the mask value and, with the
    // Filter, the levels (at most 4 times the image; only the part the levels need is
    // written, the rest never touched).
    float *shift = malloc(pixels * 4 * sizeof(float));
    float *mask_value = s->has_mask ? malloc(pixels * sizeof(float)) : NULL;
    const size_t levels_floats = s->filter ? (size_t)levels_size(width, max_levels(width)) *
                                                 levels_size(height, max_levels(height)) * 4
                                           : 0;
    float *levels = levels_floats > 0 ? malloc(levels_floats * sizeof(float)) : NULL;
    float largest[2] = {0.0f, 0.0f};
    int ok = shift && (mask_value || !s->has_mask) && (levels || !s->filter);
    if (ok) {
        MsImage bsource, blens, bmask, boutput, bshift, bmask_value, blargest, blevels;
        const MsInput *source = &inputs[SOURCE];
        // Without a Lens or a Mask, the Source stands in (only read without a Lens; the same
        // image: on the GPU it is uploaded once).
        const MsInput *lens = &inputs[s->has_lens ? LENS : SOURCE];
        const MsInput *mask = &inputs[s->has_mask ? MASK : SOURCE];
        // A Lens or a Mask that is the Source's image itself (the host gives the same
        // pixels): the Source stands in, uploaded once.
        const int lens_is_source = s->has_lens && ms_same_input(lens, source);
        const int mask_is_source = s->has_mask && ms_same_input(mask, source);
        ms_image_rgba(&bsource, source->pixels, source->row, half, width, height);
        ms_image_rgba(&blens, lens->pixels, lens->row, half, width, height);
        ms_image_rgba(&bmask, mask->pixels, mask->row, half, width, height);
        ms_image_rgba(&boutput, r->output, r->output_row, half, width, height);
        ms_image(&bshift, shift, (ptrdiff_t)width * 4, width, height, 4);
        const float no_mask = 1.0f; // without a mask: one value for every pixel (strides 0)
        if (mask_value)
            ms_image(&bmask_value, mask_value, width, width, height, 1);
        else
            ms_image_constant(&bmask_value, &no_mask, width, height);
        ms_image(&blargest, largest, 2, 2, 1, 1);
        blargest.b.dimensions = 1;
        ms_image(&blevels, levels, 4, 1, 1, 4); // sized by run()
        Job job = {.s = s,
                   .source = &bsource,
                   .lens = s->has_lens && !lens_is_source ? &blens : &bsource,
                   .mask = mask_is_source ? &bsource : &bmask,
                   .output = &boutput,
                   .shift = &bshift,
                   .mask_value = &bmask_value,
                   .largest = &blargest,
                   .levels = &blevels,
                   .width = width,
                   .height = height};
        // Uploaded: the Source, the Lens and Mask standing in (ms_run uploads an image given
        // twice once); without a mask, its one value (with one, it is made there).
        MsImage *const in[] = {&bsource, job.lens, s->has_mask ? job.mask : &bmask_value};
        MsImage *const between[] = {&bshift, &bmask_value, &blargest, &blevels};
        ok = ms_run(run, &job, in, ARRAY_SIZE(in), between, ARRAY_SIZE(between), &boutput, r->info,
                    r->err, r->err_size);
    } else {
        snprintf(r->err, r->err_size, "not enough memory");
    }
    free(shift);
    free(mask_value);
    free(levels);
    return ok;
}

// ---------------------------------------------------------------- render

static int render(const MsRender *r) {
    const double *v = r->values;
    const double w = r->frame_width; // a width of 1.0 in pixels
    Settings s;
    // Shift = Amount x 0.005 x W x (lens slope per pixel x W).
    s.amount = v[AMOUNT] * 0.005 * w * w;
    if (v[FINE])
        s.amount /= 100.0;
    s.rel_x = v[REL_X];
    s.rel_y = v[REL_Y];
    const double angle = v[ROTATE] * M_PI / 180.0;
    s.cos_r = cos(angle);
    s.sin_r = sin(angle);
    s.blur_lens = v[BLUR_LENS];
    s.blur_mask = v[BLUR_MASK];
    s.crop_l = v[CROP_LEFT];
    s.crop_r = v[CROP_RIGHT];
    s.crop_b = v[CROP_BOTTOM];
    s.crop_t = v[CROP_TOP];
    s.wrap_x = (int)v[WRAP_X];
    s.wrap_y = (int)v[WRAP_Y];
    s.filter = (int)v[FILTER];
    s.has_lens = r->connected[LENS];
    s.has_mask = r->connected[MASK];
    s.mask_alpha = (int)v[MASK_USE];
    s.invert_mask = (int)v[INVERT_MASK];
    s.mask_lens = (int)v[MASK_LENS];
    s.pre_effect = v[APPLY_MASK] == 1;
    s.gpu = (int)v[ENABLE_GPU];
    return process(r, &s);
}

const MsEffect ms_distort_effect = {
    MS_ID("Distort"),
    "MS_Distort",
    "Warps the Source by the slopes of the Lens input, like looking through textured glass.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
    NULL,
};
