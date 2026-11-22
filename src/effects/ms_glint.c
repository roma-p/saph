// MS_Glint: star-shaped rays from the bright parts of the image
// (src/halide/glint_generator.cpp).
//   - the glints' source: the Source (towards its alpha by Glint From Alpha, times the Mask by
//     its luma, RGB or first channel), blurred by Threshold Blur (3 boxes per axis, subpixel,
//     mirrored), each channel above Threshold (+ Threshold Add Color) rescaled to 0..1, the
//     pixel's colour kept
//   - rays in 4 directions (x, y, the two diagonals; Swap Diagonals swaps those): per channel a
//     3-box blur along the direction of total radius Size x Size X / Y / Diag x Size Red /
//     Green / Blue, times 4 x Brightness x Brightness X / Y / Diag x Size X / Y / Diag (the
//     diagonals x 1.333) x Scale Colors, in [0, 1]; the directions screened together
//   - Hue Shift (the hue turned, the luma kept), Blur Glint, then screened over the Source
//     scaled by Source Opacity (Glint Under Source: towards the Source over the rays); Affect
//     Alpha screens the alpha with the rays' largest channel
// Size, Threshold Blur and Blur Glint are "normalised" values, so hosts show them in pixels.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
};
enum { SOURCE, MASK };

static const char *const mask_type_entries[] = {"Luma", "Color", "Alpha"};

enum {
    BRIGHTNESS,
    SCALE_COLORS,
    SCALE_COLORS_G,
    SCALE_COLORS_B,
    BRIGHTNESS_X,
    BRIGHTNESS_Y,
    BRIGHTNESS_DIAG1,
    BRIGHTNESS_DIAG2,
    THRESHOLD,
    ADD_COLOR,
    ADD_COLOR_G,
    ADD_COLOR_B,
    THRESHOLD_BLUR,
    SIZE,
    SIZE_X,
    SIZE_Y,
    SIZE_DIAG1,
    SIZE_DIAG2,
    SIZE_RED,
    SIZE_GREEN,
    SIZE_BLUE,
    BLUR_GLINT,
    HUE_SHIFT,
    AFFECT_ALPHA,
    FROM_ALPHA,
    UNDER_SOURCE,
    SOURCE_OPACITY,
    INVERT_MASK,
    MASK_TYPE,
    SWAP_DIAGONALS,
    SHOW_SIZE,
};
static const MsParam params[] = {
    MS_RELATIVE(BRIGHTNESS, "brightness", "Brightness", "Scales the brightness of all the rays.",
                1.0),
    MS_COLOUR(SCALE_COLORS, "scale_colors", "Scale Colors", "Scales the color of the rays."),
    MS_RELATIVE(BRIGHTNESS_X, "brightness_x", "Brightness X", "Scales the horizontal rays.", 1.0),
    MS_RELATIVE(BRIGHTNESS_Y, "brightness_y", "Brightness Y", "Scales the vertical rays.", 1.0),
    MS_RELATIVE(BRIGHTNESS_DIAG1, "brightness_diag1", "Brightness Diag1",
                "Scales the rays up to the right.", 1.0),
    MS_RELATIVE(BRIGHTNESS_DIAG2, "brightness_diag2", "Brightness Diag2",
                "Scales the rays down to the right.", 1.0),
    [THRESHOLD] = {.type = MS_PARAM_DOUBLE,
                   .name = "threshold",
                   .label = "Threshold",
                   .hint = "Rays come from the parts of the image brighter than this.",
                   .def = 0.7,
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 1.0},
    MS_COLOUR_OF(ADD_COLOR, "threshold_add_color", "Threshold Add Color",
                 "Raises the threshold of each color channel by this.", 0.0),
    MS_WIDTH(THRESHOLD_BLUR, "threshold_blur", "Threshold Blur",
             "Blurs the image before the threshold: fewer rays from fine "
             "details.",
             0.0028, 0.1),
    MS_WIDTH(SIZE, "size", "Size", "The length of the rays.", 0.25, 0.5),
    MS_RELATIVE(SIZE_X, "size_x", "Size X", "Scales the length of the horizontal rays.", 1.0),
    MS_RELATIVE(SIZE_Y, "size_y", "Size Y", "Scales the length of the vertical rays.", 1.0),
    MS_RELATIVE(SIZE_DIAG1, "size_diag1", "Size Diag1",
                "Scales the length of the rays up to the right.", 0.75),
    MS_RELATIVE(SIZE_DIAG2, "size_diag2", "Size Diag2",
                "Scales the length of the rays down to the right.", 0.75),
    MS_RELATIVE(SIZE_RED, "size_red", "Size Red", "Scales the length of the red rays.", 0.5),
    MS_RELATIVE(SIZE_GREEN, "size_green", "Size Green", "Scales the length of the green rays.",
                1.0),
    MS_RELATIVE(SIZE_BLUE, "size_blue", "Size Blue", "Scales the length of the blue rays.", 1.5),
    MS_WIDTH(BLUR_GLINT, "blur_glint", "Blur Glint", "Blurs the rays.", 0.0, 0.1),
    [HUE_SHIFT] = {.type = MS_PARAM_DOUBLE,
                   .name = "hue_shift",
                   .label = "Hue Shift",
                   .hint = "Turns the hue of the rays, in turns of the color wheel.",
                   .min = -MS_UNBOUNDED,
                   .max = MS_UNBOUNDED,
                   .display_min = -1.0,
                   .display_max = 1.0},
    MS_FRACTION(AFFECT_ALPHA, "affect_alpha", "Affect Alpha", "Adds the rays to the alpha.", 0.0),
    MS_FRACTION(FROM_ALPHA, "glint_from_alpha", "Glint From Alpha",
                "Takes the rays from the alpha instead of the colors.", 0.0),
    MS_FRACTION(UNDER_SOURCE, "glint_under_source", "Glint Under Source",
                "Puts the rays behind the Source, by its alpha.", 0.0),
    MS_FRACTION(SOURCE_OPACITY, "source_opacity", "Source Opacity",
                "Scales the Source under the rays.", 1.0),
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
    [MASK_TYPE] = {.type = MS_PARAM_CHOICE,
                   .name = "mask_type",
                   .label = "Mask Type",
                   .hint = "How the Mask scales the rays' source: by its luma, its colors, or "
                           "its first channel.",
                   .entries = mask_type_entries,
                   .entry_count = ARRAY_SIZE(mask_type_entries)},
    [SWAP_DIAGONALS] = {.type = MS_PARAM_BOOL,
                        .name = "swap_diagonals",
                        .label = "Swap Diagonals",
                        .hint = "Swaps the settings of the two diagonals."},
    MS_SHOW(SHOW_SIZE, "ShowSize", "Show Size"),
};

// The kernel of each direction: x, y, up to the right, down to the right.
static const MsKernel ray_kernels[4] = {MS_K_GLINT_RAY_X, MS_K_GLINT_RAY_Y, MS_K_GLINT_RAY_UP,
                                        MS_K_GLINT_RAY_DOWN};

// One render: images and settings of the kernels (src/halide/glint_generator.cpp).
typedef struct {
    MsImage *source, *mask, *glints, *totals[2], *output;
    int has_mask, invert_mask, mask_type, blur, glint_blur;
    float blur_frac, glint_blur_frac, threshold, add[3], from_alpha;
    int used[4];     // the directions with rays
    int r[4][3];     // total radius per direction and channel
    float amp[4][3]; // amplitude per direction and channel
    float hue, source_opacity, under, affect_alpha;
} Job;

// The glints' source, the rays of each direction (screened into totals[0] / [1] in turn),
// then the result, on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    int done = 0, status = 0;
    for (int d = 0; d < 4 && status == 0; ++d) {
        if (!j->used[d])
            continue;
        if (done == 0)
            status =
                MS_KERNEL(MS_K_GLINT_SOURCE, api, MS_IMAGE("source", j->source),
                          MS_IMAGE("mask", j->mask), MS_INT("has_mask", j->has_mask),
                          MS_INT("invert_mask", j->invert_mask), MS_INT("mask_type", j->mask_type),
                          MS_FLOAT("from_alpha", j->from_alpha), MS_INT("blur", j->blur),
                          MS_FLOAT("blur_frac", j->blur_frac), MS_FLOAT("threshold", j->threshold),
                          MS_FLOAT("add_r", j->add[0]), MS_FLOAT("add_g", j->add[1]),
                          MS_FLOAT("add_b", j->add[2]), MS_IMAGE("glints", j->glints));
        if (status != 0)
            break;
        status = MS_KERNEL(ray_kernels[d], api, MS_IMAGE("glints", j->glints),
                           MS_IMAGE("total", j->totals[(done + 1) % 2]), MS_INT("first", done == 0),
                           MS_INT("r_r", j->r[d][0]), MS_INT("r_g", j->r[d][1]),
                           MS_INT("r_b", j->r[d][2]), MS_FLOAT("amp_r", j->amp[d][0]),
                           MS_FLOAT("amp_g", j->amp[d][1]), MS_FLOAT("amp_b", j->amp[d][2]),
                           MS_IMAGE("out", j->totals[done % 2]));
        ++done;
    }
    if (status != 0)
        return status;
    const int blurred = j->glint_blur > 0 || j->glint_blur_frac > 0.0f;
    return MS_KERNEL(blurred ? MS_K_GLINT_COMBINE_BLUR : MS_K_GLINT_COMBINE, api,
                     MS_IMAGE("source", j->source),
                     MS_IMAGE("total", done ? j->totals[(done - 1) % 2] : j->source),
                     MS_INT("has_rays", done > 0), MS_FLOAT("hue", j->hue),
                     MS_INT("blur", j->glint_blur), MS_FLOAT("blur_frac", j->glint_blur_frac),
                     MS_FLOAT("source_opacity", j->source_opacity), MS_FLOAT("under", j->under),
                     MS_FLOAT("affect_alpha", j->affect_alpha), MS_IMAGE("output", j->output));
}

// A width in pixels: whole pixels and the fraction past them.
static void split(double v, int *whole, float *frac) {
    v = v > 0 ? (v < 1e7 ? v : 1e7) : 0;
    *whole = (int)floor(v);
    *frac = (float)(v - *whole);
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height, has_mask = r->connected[MASK];
    Job j = {.has_mask = has_mask,
             .invert_mask = (int)v[INVERT_MASK],
             .mask_type = (int)v[MASK_TYPE],
             .threshold = (float)v[THRESHOLD],
             .from_alpha = (float)v[FROM_ALPHA],
             .hue = (float)(v[HUE_SHIFT] - floor(v[HUE_SHIFT])),
             .source_opacity = (float)v[SOURCE_OPACITY],
             .under = (float)v[UNDER_SOURCE],
             .affect_alpha = (float)v[AFFECT_ALPHA]};
    split(v[THRESHOLD_BLUR], &j.blur, &j.blur_frac);
    split(v[BLUR_GLINT], &j.glint_blur, &j.glint_blur_frac);
    for (int c = 0; c < 3; ++c)
        j.add[c] = (float)v[ADD_COLOR + c];
    // Per direction: its Size and Brightness (the diagonals' swapped by Swap Diagonals).
    const int swap = (int)v[SWAP_DIAGONALS];
    int rays = 0;
    for (int d = 0; d < 4; ++d) {
        const int s = d < 2 || !swap ? d : 5 - d;
        const double size = v[SIZE_X + s], gain = 4.0 * v[BRIGHTNESS] * v[BRIGHTNESS_X + s] * size *
                                                  (d < 2 ? 1.0 : 1.333);
        for (int c = 0; c < 3; ++c) {
            double t = v[SIZE] * size * v[SIZE_RED + c];
            t = t > 0 ? (t < 1e7 ? t : 1e7) : 0;
            j.r[d][c] = (int)floor(t + 0.5);
            j.amp[d][c] = (float)(gain * v[SCALE_COLORS + c]);
            j.used[d] |= j.amp[d][c] != 0.0f;
        }
        rays += j.used[d];
    }
    // The glints' source and two totals, between the kernels.
    const size_t image = (size_t)w * h * 4;
    const size_t floats[] = {rays ? image : 0, rays ? image : 0, rays > 1 ? image : 0};
    float *block = malloc(ms_parts_size(floats, ARRAY_SIZE(floats))), *part[ARRAY_SIZE(floats)];
    if (!block)
        return ms_out_of_memory(r);
    ms_parts(block, floats, ARRAY_SIZE(floats), part);
    MsImage source, mask, glints_image, total_images[2], output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&mask, r->inputs[MASK].pixels, r->inputs[MASK].row, r->half, w, h);
    ms_image(&glints_image, part[0], (ptrdiff_t)w * 4, w, h, 4);
    ms_image(&total_images[0], part[1], (ptrdiff_t)w * 4, w, h, 4);
    ms_image(&total_images[1], part[rays > 1 ? 2 : 1], (ptrdiff_t)w * 4, w, h, 4);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    // Without a Mask, or one that is the Source's image in the host: the Source stands in
    // (uploaded once).
    const int mask_is_source = has_mask && ms_same_input(&r->inputs[MASK], &r->inputs[SOURCE]);
    j.source = &source;
    j.mask = has_mask && !mask_is_source ? &mask : &source;
    j.glints = &glints_image;
    j.totals[0] = &total_images[0];
    j.totals[1] = &total_images[1];
    j.output = &output;
    // Inputs uploaded to the GPU: the Source, and the Mask.
    MsImage *const inputs[] = {&source, j.mask};
    MsImage *const between[] = {rays ? &glints_image : NULL, rays ? &total_images[0] : NULL,
                                rays > 1 ? &total_images[1] : NULL};
    const int ok = ms_run(run, &j, inputs, ARRAY_SIZE(inputs), between, ARRAY_SIZE(between),
                          &output, r->info, r->err, r->err_size);
    free(block);
    return ok;
}

// The viewer: the rays both ways (x: Size x Size X; a diagonal's end Size x Size Diag away
// on both axes), the Size (white, 0.9 x Size) and the colours' sizes.
static const MsOverlay overlay[] = {
    {.kind = MS_OVERLAY_RAY,
     .colour = MS_OVERLAY_ORANGE,
     .show = "ShowSize",
     .size = "size",
     .scale = "size_x",
     .dir = {1, 0}},
    {.kind = MS_OVERLAY_RAY,
     .colour = MS_OVERLAY_ORANGE,
     .show = "ShowSize",
     .size = "size",
     .scale = "size_y",
     .dir = {0, 1}},
    {.kind = MS_OVERLAY_RAY,
     .colour = MS_OVERLAY_ORANGE,
     .show = "ShowSize",
     .size = "size",
     .scale = "size_diag1",
     .dir = {1, 1}},
    {.kind = MS_OVERLAY_RAY,
     .colour = MS_OVERLAY_ORANGE,
     .show = "ShowSize",
     .size = "size",
     .scale = "size_diag2",
     .dir = {-1, 1}},
    {.kind = MS_OVERLAY_DOT, .colour = MS_OVERLAY_ORANGE, .show = "ShowSize"},
    MS_OVERLAY_SIZED(MS_OVERLAY_ELLIPSE, "ShowSize", "size", "size_red", NULL, NULL, 0.0,
                     MS_OVERLAY_RED),
    MS_OVERLAY_SIZED(MS_OVERLAY_ELLIPSE, "ShowSize", "size", NULL, NULL, NULL, 0.9,
                     MS_OVERLAY_WHITE),
    MS_OVERLAY_SIZED(MS_OVERLAY_ELLIPSE, "ShowSize", "size", "size_green", NULL, NULL, 0.0,
                     MS_OVERLAY_GREEN),
    MS_OVERLAY_SIZED(MS_OVERLAY_ELLIPSE, "ShowSize", "size", "size_blue", NULL, NULL, 0.0,
                     MS_OVERLAY_BLUE),
};

const MsEffect ms_glint_effect = {
    .id = MS_ID("Glint"),
    .label = "MS_Glint",
    .description = "Star-shaped rays from the parts of the image brighter than a threshold.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    MS_OVERLAYS(overlay),
    .presets = "glint",
};
