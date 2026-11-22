// MS_PixelSort: the pixels sorted by their value along lines, in the spans a threshold
// leaves (src/halide/pixel_sort_generator.cpp).
//   - Mode: Linear (straight lines at the Sort Angle), Radial (rays from the Center, from
//     Inner Radius over Ray Length; Vary Radius: each ray's start moved out by up to that share
//     of the distance to the image's farthest corner, its end kept),
//     Circular (rings around the Circle Center from Inner Radius over Thickness; Vary Start:
//     each ring's start turned by up to that share of a turn); radii 2 x the settings (as the
//     reference's); Start Angle and Degrees Sorted limit both to a sector
//   - Sort Type: the value sorted (monochrome 5/16 R + 8/16 G + 3/16 B, average, minimum,
//     maximum, a channel, HSV hue, saturation, brightness); Threshold, Sort Direction (the
//     spans: values above or below it), Soften Threshold Mask (3 box blurs: a soft mask, each
//     line's spans where it is above the line's own random threshold), Reverse
//   - Randomly Restart Sort r: a share p(r / width) of the pixels never move and split the
//     spans (the reference's share, measured)
//   - Downsample: sorted at Sort Resolution x round(Sort Resolution x height / width)
//   - Seed: MillSaph's own random (the lines' thresholds, the restarts, Vary)
//   - Mask post-threshold: times the threshold mask; pre-effect: mixes the result over the
//     Source. Mix With Source: the result mixed with it
// Not here: the Mocha settings, Soft Borders.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/input.h"
#include "effects/mask_input.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
};
enum { SOURCE, MASK };

static const char *const modes[] = {"Linear", "Radial", "Circular"};
static const char *const apply[] = {"Post-threshold", "Pre-effect"};
static const char *const directions[] = {"sort below threshold", "sort above threshold"};
static const char *const types[] = {"monochrome", "average", "minimum", "maximum",    "red",
                                    "green",      "blue",    "hue",     "saturation", "brightness"};
static const char *const shows[] = {"Result", "Raw Sort Values", "Threshold Mask",
                                    "Random Restart Noise", "Combined Threshold Mask"};
enum {
    MODE,
    APPLY_MASK,
    SORT_ANGLE,
    CENTER,
    CENTER_Y,
    RAY_START,
    RAY_DEGREES,
    RAY_INNER,
    RAY_LENGTH,
    VARY_RADIUS,
    RING_START,
    RING_DEGREES,
    RING_CENTER,
    RING_CENTER_Y,
    VARY_START,
    RING_INNER,
    THICKNESS,
    THRESHOLD,
    SORT_DIRECTION,
    REVERSE,
    SORT_TYPE,
    RESTART,
    SOFTEN,
    DOWNSAMPLE,
    SORT_RESOLUTION,
    SEED,
    MIX,
    SHOW,
    MASK_USE,
    BLUR_MASK,
    INVERT_MASK
};
#define START_ANGLE(id, n, h)                                                                      \
    [id] = {.type = MS_PARAM_ANGLE,                                                                \
            .name = (n),                                                                           \
            .label = "Start Angle",                                                                \
            .hint = (h),                                                                           \
            .min = -MS_UNBOUNDED,                                                                  \
            .max = MS_UNBOUNDED,                                                                   \
            .display_min = -360.0,                                                                 \
            .display_max = 360.0}
#define DEGREES(id, n, h, d)                                                                       \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = "Degrees Sorted",                                                             \
            .hint = (h),                                                                           \
            .def = (d),                                                                            \
            .min = 0.0,                                                                            \
            .max = 360.0,                                                                          \
            .display_max = 360.0}
static const MsParam params[] = {
    [MODE] = {.type = MS_PARAM_CHOICE,
              .name = "Mode",
              .label = "Mode",
              .hint = "The lines sorted: straight, rays from a centre, or rings around it.",
              .entries = modes,
              .entry_count = 3},
    [APPLY_MASK] = {.type = MS_PARAM_CHOICE,
                    .name = "apply_mask",
                    .label = "Apply Mask",
                    .hint = "Post-threshold multiplies the threshold mask by the Mask; "
                            "Pre-effect mixes the result over the Source by it.",
                    .entries = apply,
                    .entry_count = 2},
    [SORT_ANGLE] = {.type = MS_PARAM_ANGLE,
                    .name = "sort_angle",
                    .label = "Sort Angle",
                    .hint = "The direction of the lines (Linear; degrees, counter-clockwise from "
                            "the right).",
                    .min = -MS_UNBOUNDED,
                    .max = MS_UNBOUNDED,
                    .display_min = -360.0,
                    .display_max = 360.0},
    [CENTER] = {.type = MS_PARAM_POINT,
                .name = "center",
                .label = "Center",
                .hint = "Where the rays start (Radial).",
                .def = 0.5},
    [CENTER_Y] = {.type = MS_PARAM_POINT_Y, .name = "center_y", .def = 0.5},
    START_ANGLE(RAY_START, "_start_angle", "The direction of the first ray (Radial)."),
    DEGREES(RAY_DEGREES, "_degrees_sorted", "The rays' sector, from the Start Angle (Radial).",
            360.0),
    MS_DISTANCE(RAY_INNER, "_inner_radius", "Inner Radius", "Where the rays start (Radial).", 0.05),
    MS_DISTANCE(RAY_LENGTH, "ray_length", "Ray Length", "The rays' length (Radial).", 0.4),
    MS_FRACTION(VARY_RADIUS, "vary_radius", "Vary Radius",
                "Moves each ray's start outwards by a random amount, up to this share of the "
                "distance to the image's farthest corner (Radial).",
                0.1),
    START_ANGLE(RING_START, "start_angle", "Where the rings start (Circular)."),
    DEGREES(RING_DEGREES, "degrees_sorted",
            "The rings' arc, counter-clockwise from the Start Angle (Circular).", 270.0),
    [RING_CENTER] = {.type = MS_PARAM_POINT,
                     .name = "circle_center",
                     .label = "Circle Center",
                     .hint = "The rings' centre (Circular).",
                     .def = 0.5},
    [RING_CENTER_Y] = {.type = MS_PARAM_POINT_Y, .name = "circle_center_y", .def = 0.5},
    MS_FRACTION(VARY_START, "vary_start", "Vary Start",
                "Turns each ring's start by a random share of a turn up to this (Circular).", 0.15),
    MS_DISTANCE(RING_INNER, "inner_radius", "Inner Radius", "The smallest ring (Circular).", 0.0),
    MS_DISTANCE(THICKNESS, "thickness", "Thickness", "The rings' width (Circular).", 0.55),
    MS_FRACTION(THRESHOLD, "threshold", "Threshold",
                "The pixels sorted: those whose value is above (or below) it.", 0.3),
    [SORT_DIRECTION] = {.type = MS_PARAM_CHOICE,
                        .name = "sort_direction",
                        .label = "Sort Direction",
                        .hint = "Sorts the pixels above the Threshold, or those below.",
                        .def = 1,
                        .entries = directions,
                        .entry_count = 2},
    [REVERSE] = {.type = MS_PARAM_BOOL,
                 .name = "reverse_sort_direction",
                 .label = "Reverse Sort Direction",
                 .hint = "Sorts from the largest value to the smallest."},
    [SORT_TYPE] = {.type = MS_PARAM_CHOICE,
                   .name = "sort_type",
                   .label = "Sort Type",
                   .hint = "The value of the pixels sorted by.",
                   .entries = types,
                   .entry_count = 10},
    [RESTART] = {.type = MS_PARAM_DOUBLE,
                 .name = "randomly_restart_sort",
                 .label = "Randomly Restart Sort",
                 .hint = "Random pixels that stay and split the sorted runs (0: none).",
                 .def = 100.0,
                 .min = 0.0,
                 .max = MS_UNBOUNDED,
                 .display_max = 1000.0},
    MS_DISTANCE(SOFTEN, "soften_threshold_mask", "Soften Threshold Mask",
                "Blurs the threshold mask: the runs then end at random within its edges.", 0.0125),
    [DOWNSAMPLE] = {.type = MS_PARAM_BOOL,
                    .name = "downsample",
                    .label = "Downsample",
                    .hint = "Sorts the image at the Sort Resolution: blocky runs, faster."},
    [SORT_RESOLUTION] = {.type = MS_PARAM_DOUBLE,
                         .name = "sort_resolution",
                         .label = "Sort Resolution",
                         .hint = "The width the image is sorted at (Downsample).",
                         .def = 720.0,
                         .min = 1.0,
                         .max = MS_UNBOUNDED,
                         .display_min = 1.0,
                         .display_max = 1920.0},
    [SEED] = {.type = MS_PARAM_DOUBLE,
              .name = "seed",
              .label = "Seed",
              .hint = "Another random pattern for each value.",
              .def = 0.273,
              .min = -MS_UNBOUNDED,
              .max = MS_UNBOUNDED,
              .display_max = 5.0},
    MS_FRACTION(MIX, "mix_with_source", "Mix With Source", "Mixes the Source back in.", 0.0),
    [SHOW] = {.type = MS_PARAM_CHOICE,
              .name = "show",
              .label = "Show",
              .hint = "The result, or what makes it: the values sorted, the masks, the restart "
                      "pixels.",
              .entries = shows,
              .entry_count = 5},
    // (the reference reads the Mask's luma whatever its Mask Use says: kept for the scripts
    // and presets that set it, not shown)
    [MASK_USE] = MS_PARAM_MASK_USE_HIDDEN(1),
    [BLUR_MASK] = MS_PARAM_BLUR_MASK(0.00625, 0.2),
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Sorts the black areas of the Mask."),
};

// The reference's radii: 2 x its settings.
#define RADIUS_SCALE 2.0

// The bitonic passes at distances under this are run block by block (pixel_sort_generator.cpp's
// BLOCK).
#define PIXEL_SORT_BLOCK 512

// The share of restart pixels by Randomly Restart Sort / width (the reference's, measured;
// linear between, held past the last).
static double restart_share(double rate) {
    static const double at[][2] = {{0.0, 0.0},      {0.039, 0.0009}, {0.098, 0.0053},
                                   {0.195, 0.0069}, {0.29, 0.0103},  {0.39, 0.0117},
                                   {0.59, 0.012}};
    const int n = ARRAY_SIZE(at);
    if (rate <= 0.0)
        return 0.0;
    for (int k = 1; k < n; ++k)
        if (rate < at[k][0])
            return at[k - 1][1] +
                   (at[k][1] - at[k - 1][1]) * (rate - at[k - 1][0]) / (at[k][0] - at[k - 1][0]);
    return at[n - 1][1];
}

static int power_of_2(double n) {
    int p = 1;
    while (p < n && p < 65536)
        p <<= 1;
    return p;
}

// One render: images and settings of the kernels (src/halide/pixel_sort_generator.cpp,
// mask_generator.cpp).
typedef struct {
    MsImage *source, *image, *soft, *lines[2], *output;
    MsMask *mask;
    int downsample, reach_x, reach_y;
    int mode, major, dir, offset, seed, sort_type, mask_post, mask_pre, above, reverse, ends;
    int soften[3], samples, count, show;
    float slope, cx, cy, inner, extent, vary, a0, span, step, restart, threshold, mix;
} Job;

#define LINES_ARGS(j)                                                                              \
    MS_INT("mode", (j)->mode), MS_INT("major", (j)->major), MS_INT("dir", (j)->dir),               \
        MS_FLOAT("slope", (j)->slope), MS_INT("offset", (j)->offset), MS_FLOAT("cx", (j)->cx),     \
        MS_FLOAT("cy", (j)->cy), MS_FLOAT("inner", (j)->inner), MS_FLOAT("extent", (j)->extent),   \
        MS_FLOAT("vary", (j)->vary), MS_FLOAT("a0", (j)->a0), MS_FLOAT("span", (j)->span),         \
        MS_FLOAT("step", (j)->step), MS_INT("seed", (j)->seed), MS_FLOAT("restart", (j)->restart), \
        MS_INT("sort_type", (j)->sort_type), MS_INT("mask_post", (j)->mask_post)

static int run(MsApi api, void *user) {
    const Job *j = user;
    int status = ms_mask_run(j->mask, api);
    if (status == 0 && j->downsample)
        status = MS_KERNEL(MS_K_PIXEL_SORT_DOWN, api, MS_IMAGE("source", j->source),
                           MS_INT("reach_x", j->reach_x), MS_INT("reach_y", j->reach_y),
                           MS_IMAGE("image", j->image));
    if (status == 0)
        status = MS_KERNEL(MS_K_PIXEL_SORT_PREP, api, MS_IMAGE("image", j->image),
                           MS_IMAGE("mask_value", &j->mask->value), LINES_ARGS(j),
                           MS_FLOAT("threshold", j->threshold), MS_INT("above", j->above),
                           MS_INT("reverse", j->reverse), MS_INT("ends", j->ends),
                           MS_INT("soften1", j->soften[0]), MS_INT("soften2", j->soften[1]),
                           MS_INT("soften3", j->soften[2]), MS_INT("samples", j->samples),
                           MS_INT("count", j->count), MS_IMAGE("lines", j->lines[0]),
                           MS_IMAGE("soft", j->soft));
    // The bitonic network: blocks of k samples, each merged by passes at distances k / 2 .. 1
    // (those under PIXEL_SORT_BLOCK in one call).
    int at = 0;
    for (int k = 2; status == 0 && j->show == 0 && k <= j->samples; k <<= 1)
        for (int d = k >> 1; status == 0 && d > 0; d >>= 1) {
            const int block = d < PIXEL_SORT_BLOCK;
            status = MS_KERNEL(block ? MS_K_PIXEL_SORT_BLOCK : MS_K_PIXEL_SORT_STEP, api,
                               MS_IMAGE("before", j->lines[at]), MS_INT("k", k), MS_INT("j", d),
                               MS_IMAGE("after", j->lines[1 - at]));
            at = 1 - at;
            if (block)
                break;
        }
    if (status == 0)
        status = MS_KERNEL(MS_K_PIXEL_SORT_OUT, api, MS_IMAGE("source", j->source),
                           MS_IMAGE("image", j->image), MS_IMAGE("soft", j->soft),
                           MS_IMAGE("lines", j->lines[at]), MS_IMAGE("mask_value", &j->mask->value),
                           LINES_ARGS(j), MS_INT("show", j->show), MS_INT("mask_pre", j->mask_pre),
                           MS_FLOAT("mix", j->mix), MS_IMAGE("output", j->output));
    return status;
}

// The lines of the image sorted (sw x sh): their kind and size.
static void lines(const double *v, int w, int sw, int sh, Job *j) {
    const double scale = (double)sw / w;
    j->mode = (int)v[MODE];
    j->count = 1;
    j->samples = 1;
    if (j->mode == 0) {
        // Digital lines: along the major axis one pixel at a time, across floor(slope a + 0.5).
        double angle = fmod(v[SORT_ANGLE], 360.0);
        angle += angle < 0.0 ? 360.0 : 0.0;
        const double eighths = angle / 45.0, nearest = floor(eighths + 0.5);
        const int on_eighth = fabs(eighths - nearest) < 1e-9;
        const int e = (int)nearest % 8;
        double dx = cos(angle * M_PI / 180.0), dy = sin(angle * M_PI / 180.0);
        if (on_eighth) { // (exactly: the diagonals' slope 1)
            static const int ex[8] = {1, 1, 0, -1, -1, -1, 0, 1},
                             ey[8] = {0, 1, 1, 1, 0, -1, -1, -1};
            dx = ex[e];
            dy = ey[e];
        }
        j->major = fabs(dx) >= fabs(dy) ? 0 : 1;
        j->dir = (j->major == 0 ? dx : dy) > 0.0 ? 1 : -1;
        j->slope = (float)(j->major == 0 ? dy / dx : dx / dy);
        const int n = j->major == 0 ? sw : sh, across = j->major == 0 ? sh : sw;
        // floor(slope a + 0.5) at both ends (monotonic), with a line to spare each side
        const int o1 = (int)floorf(j->slope * (float)(n - 1) + 0.5f);
        const int lo = o1 < 0 ? o1 : 0, hi = o1 > 0 ? o1 : 0;
        j->offset = hi + 1;
        j->count = across + hi - lo + 2;
        j->samples = power_of_2(n);
        j->ends = on_eighth && e % 2 == 0 ? 1 : 2;
        return;
    }
    const int radial = j->mode == 1;
    const double cx = v[radial ? CENTER : RING_CENTER] * scale;
    const double cy = v[radial ? CENTER_Y : RING_CENTER_Y] * scale;
    const double inner = fmax(v[radial ? RAY_INNER : RING_INNER], 0.0) * RADIUS_SCALE * scale;
    const double extent = fmax(v[radial ? RAY_LENGTH : THICKNESS], 0.0) * RADIUS_SCALE * scale;
    // (only as far as the image's farthest corner)
    const double far =
        sqrt(fmax(cx * cx, (sw - cx) * (sw - cx)) + fmax(cy * cy, (sh - cy) * (sh - cy)));
    const double reach = fmax(fmin(extent, far - inner), 0.0);
    const double span =
        fmin(fmax(v[radial ? RAY_DEGREES : RING_DEGREES], 0.0), 360.0) * M_PI / 180.0;
    j->cx = (float)cx;
    j->cy = (float)cy;
    j->inner = (float)inner;
    j->extent = (float)extent;
    j->a0 = (float)(v[radial ? RAY_START : RING_START] * M_PI / 180.0);
    j->span = (float)span;
    j->vary = (float)fmin(fmax(v[radial ? VARY_RADIUS : VARY_START], 0.0), 1.0);
    j->ends = 1;
    if (radial) {
        // as many rays as pixels on the outer circle
        j->vary *= (float)far; // (the largest move out, in pixels)
        const double outer = fmin(inner + extent, far);
        j->count = (int)fmin(fmax(ceil(span * outer), 1.0), 65536.0);
        j->step = (float)(span / j->count);
        j->samples = power_of_2(ceil(reach));
    } else {
        j->count = (int)fmax(ceil(reach), 1.0);
        j->samples = power_of_2(ceil(span * (inner + reach)) + 1.0);
    }
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height, has_mask = r->connected[MASK];
    Job j = {.seed = (int)floor(fmod(v[SEED] * 1000.0, 2147483647.0)),
             .sort_type = (int)v[SORT_TYPE],
             .mask_post = has_mask && (int)v[APPLY_MASK] == 0,
             .mask_pre = has_mask && (int)v[APPLY_MASK] == 1,
             .above = (int)v[SORT_DIRECTION],
             .reverse = (int)v[REVERSE],
             .show = (int)v[SHOW],
             .threshold = (float)v[THRESHOLD],
             .mix = (float)fmin(fmax(v[MIX], 0.0), 1.0)};
    // The image sorted: the Source, or its cells at the Sort Resolution.
    int sw = w, sh = h;
    if (v[DOWNSAMPLE] != 0.0) {
        const double res = fmin(fmax(floor(v[SORT_RESOLUTION] + 0.5), 1.0), (double)w);
        sw = (int)res;
        sh = (int)fmin(fmax(floor(res * h / w + 0.5), 1.0), (double)h);
    }
    j.downsample = sw != w || sh != h;
    j.reach_x = (int)ceil((double)w / sw) + 1;
    j.reach_y = (int)ceil((double)h / sh) + 1;
    lines(v, w, sw, sh, &j);
    j.restart = (float)restart_share(v[RESTART] / w);
    const int soften = (int)floor(fmax(v[SOFTEN], 0.0) * sw / w + 0.5);
    for (int k = 0; k < 3; ++k)
        j.soften[k] = (soften + k) / 3;

    // The downsampled Source, the softened sort values, the lines' two buffers (integers), the
    // mask value.
    const size_t pixels = (size_t)sw * sh, samples = (size_t)2 * j.samples * j.count;
    const size_t floats[] = {j.downsample ? pixels * 4 : 0, pixels, samples, samples,
                             ms_mask_floats(r, MASK)};
    float *block = malloc(ms_parts_size(floats, ARRAY_SIZE(floats))), *part[ARRAY_SIZE(floats)];
    if (!block)
        return ms_out_of_memory(r);
    ms_parts(block, floats, ARRAY_SIZE(floats), part);
    MsImage source, image, soft_image, line_images[2], output;
    ms_input_image(&source, r, SOURCE);
    ms_image(&image, part[0], (ptrdiff_t)sw * 4, sw, sh, 4);
    ms_image(&soft_image, part[1], sw, sw, sh, 1);
    for (int k = 0; k < 2; ++k) {
        ms_image_uint(&line_images[k], part[2 + k], 2 * j.samples, 2 * j.samples, j.count);
        j.lines[k] = &line_images[k];
    }
    ms_output_image(&output, r);
    MsMask mask; // (the reference reads the Mask's luma whatever Mask Use says)
    ms_mask_init(&mask, r, MASK, SOURCE, &source, part[4], 0.0, v[INVERT_MASK], v[BLUR_MASK]);
    j.source = &source;
    j.mask = &mask;
    j.image = j.downsample ? &image : &source;
    j.soft = &soft_image;
    j.output = &output;
    // Inputs uploaded to the GPU: the Source, the Mask or the one mask value.
    MsImage *const inputs[] = {&source, mask.input};
    MsImage *const between[] = {&soft_image, &line_images[0], &line_images[1],
                                ms_mask_between(&mask), j.downsample ? &image : NULL};
    const int ok = ms_run(run, &j, inputs, ARRAY_SIZE(inputs), between, ARRAY_SIZE(between),
                          &output, r->info, r->err, r->err_size);
    free(block);
    return ok;
}

// Mix With Source 1 showing the result: the Source as it is.
static int identity(const double *values, const int *connected) {
    (void)connected;
    return values[MIX] >= 1.0 && values[SHOW] == 0.0;
}

const MsEffect ms_pixelsort_effect = {
    .id = MS_ID("PixelSort"),
    .label = "MS_PixelSort",
    .description =
        "Sorts the pixels by brightness (or hue, a channel...) along lines, rays or rings, in the "
        "runs a threshold leaves.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    .identity = identity,
    .presets = "pixelsort",
};
