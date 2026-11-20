// MS_RackDefocus: a lens blur (src/halide/rack_defocus_generator.cpp), by FFT (fft.c).
//   - the Source padded by the Edge Mode (Reflect: mirrored without repeating the border
//     pixel), Defocus Mono: its luma (5 R + 8 G + 3 B) / 16; Use Gamma g: v^g between 0 and
//     1; Boost Highlights b: v + c v max(v - Hilight Threshold, 0), with
//     c = 64000 (b x Defocus Width / frame width)^2, on the colours
//   - convolved by the lens: the Shape (a regular polygon, a vertex up, Roundness bending its
//     sides; the Circle a rounded triangle) of radius Defocus Width (x Rel Height in y), each
//     pixel's coverage, weighted by Bokeh (x = (d / radius)^2: 1 - b x, or 1 + b - b x below 0),
//     its radius per channel R 2^-f, R, R 2^f by Color Fringing f, blurred by Gauss Blur, summing
//     to 1
//   - inverse gamma, Brightness x b + Offset Darks x (1 - b) on the colours, then Mix With
//     Source and the Mask
// Defocus Width, Gauss Blur and Blur Mask are "normalised" values, so hosts show them in
// pixels.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/fft.h"
#include "effects/mask_input.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
};
enum { SOURCE, MASK };

static const char *const mode_entries[] = {"Defocus Color", "Defocus Mono"};
static const char *const shape_entries[] = {"Circle",   "3 sides",  "4 sides", "5 sides",
                                            "6 sides",  "7 sides",  "8 sides", "9 sides",
                                            "10 sides", "11 sides", "12 sides"};
static const char *const edge_entries[] = {"Transparent", "Repeat", "Reflect"};

enum {
    MODE,
    DEFOCUS_WIDTH,
    REL_HEIGHT,
    SHAPE,
    ROUNDNESS,
    BOKEH,
    GAUSS_BLUR,
    USE_GAMMA,
    BOOST,
    THRESHOLD,
    FRINGING,
    BRIGHTNESS,
    OFFSET_DARKS,
    MIX,
    EDGE_MODE,
    MASK_USE,
    BLUR_MASK,
    INVERT_MASK
};
static const MsParam params[] = {
    [MODE] = {.type = MS_PARAM_CHOICE,
              .name = "mode",
              .label = "Mode",
              .hint = "Blurs the colors, or the luma alone (a monochrome result).",
              .entries = mode_entries,
              .entry_count = ARRAY_SIZE(mode_entries)},
    [DEFOCUS_WIDTH] = {.type = MS_PARAM_WIDTH,
                       .name = "defocus_width",
                       .label = "Defocus Width",
                       .hint = "The radius of the lens.",
                       .def = 0.011,
                       .min = 0.0,
                       .max = MS_UNBOUNDED,
                       .display_max = 0.25},
    [REL_HEIGHT] = {.type = MS_PARAM_DOUBLE,
                    .name = "rel_height",
                    .label = "Rel Height",
                    .hint = "The height of the lens relative to its width.",
                    .def = 1.0,
                    .min = 0.01,
                    .max = MS_UNBOUNDED,
                    .display_min = 0.1,
                    .display_max = 5.0},
    [SHAPE] = {.type = MS_PARAM_CHOICE,
               .name = "shape",
               .label = "Shape",
               .hint = "The shape of the lens: a circle, or a polygon.",
               .entries = shape_entries,
               .entry_count = ARRAY_SIZE(shape_entries)},
    [ROUNDNESS] = {.type = MS_PARAM_DOUBLE,
                   .name = "roundness",
                   .label = "Roundness",
                   .hint = "Bends the polygon's sides: out toward a circle, or in toward a star.",
                   .min = -1.0,
                   .max = 1.0,
                   .display_min = -1.0,
                   .display_max = 1.0},
    [BOKEH] = {.type = MS_PARAM_DOUBLE,
               .name = "bokeh",
               .label = "Bokeh",
               .hint = "Brighter lens center (above 0) or rim (below 0).",
               .min = -1.0,
               .max = 1.0,
               .display_min = -1.0,
               .display_max = 1.0},
    [GAUSS_BLUR] = {.type = MS_PARAM_WIDTH,
                    .name = "gauss_blur",
                    .label = "Gauss Blur",
                    .hint = "Softens the lens' edge by this much.",
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 0.05},
    [USE_GAMMA] = {.type = MS_PARAM_DOUBLE,
                   .name = "use_gamma",
                   .label = "Use Gamma",
                   .hint = "Blurs the image raised to this power (between 0 and 1): above 1, "
                           "the bright parts spread more.",
                   .def = 1.0,
                   .min = 0.01,
                   .max = MS_UNBOUNDED,
                   .display_min = 0.1,
                   .display_max = 5.0},
    [BOOST] = {.type = MS_PARAM_DOUBLE,
               .name = "boost_highlights",
               .label = "Boost Highlights",
               .hint = "Brightens the parts above Hilight Threshold before the blur.",
               .min = 0.0,
               .max = MS_UNBOUNDED,
               .display_max = 5.0},
    [THRESHOLD] = {.type = MS_PARAM_DOUBLE,
                   .name = "hilight_threshold",
                   .label = "Hilight Threshold",
                   .hint = "Boost Highlights brightens the parts brighter than this.",
                   .def = 0.9,
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 1.0},
    [FRINGING] = {.type = MS_PARAM_DOUBLE,
                  .name = "color_fringing",
                  .label = "Color Fringing",
                  .hint = "A wider lens for blue and a narrower one for red (or the reverse "
                          "below 0).",
                  .min = -5.0,
                  .max = 5.0,
                  .display_min = -0.5,
                  .display_max = 0.5},
    [BRIGHTNESS] = {.type = MS_PARAM_DOUBLE,
                    .name = "brightness",
                    .label = "Brightness",
                    .hint = "Scales the blurred colors.",
                    .def = 1.0,
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 5.0},
    [OFFSET_DARKS] = {.type = MS_PARAM_DOUBLE,
                      .name = "offset_darks",
                      .label = "Offset Darks",
                      .hint = "Raises the dark parts of the blurred colors toward 1 (or lowers "
                              "them below 0).",
                      .min = -MS_UNBOUNDED,
                      .max = MS_UNBOUNDED,
                      .display_min = -5.0,
                      .display_max = 1.0},
    [MIX] = {.type = MS_PARAM_DOUBLE,
             .name = "mix_with_source",
             .label = "Mix With Source",
             .hint = "Mixes the Source back over the result: 0 for the blur only.",
             .min = 0.0,
             .max = 1.0,
             .display_max = 1.0},
    [EDGE_MODE] = {.type = MS_PARAM_CHOICE,
                   .name = "edge_mode",
                   .label = "Edge Mode",
                   .hint = "Outside the image: black, the border pixels repeated, or the image "
                           "mirrored.",
                   .def = 2,
                   .entries = edge_entries,
                   .entry_count = ARRAY_SIZE(edge_entries)},
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

// One render: images and settings of the kernels.
typedef struct {
    MsImage *source, *output;
    MsMask *mask;
    MsImage *buffers[3]; // the transforms' images (width x height)
    int pad, edge_mode, mono, sides, gauss, half;
    float gamma, boost, threshold, radius[3], rel_height, power, bokeh;
    float brightness, offset_darks, mix, scale;
} Job;

// The mask value (with a Mask); the lens and the Source, transformed; their product,
// transformed back; the result, on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    int status = ms_mask_run(j->mask, api);
    MsImage *lens = j->buffers[0], *image = j->buffers[1], *free_image = j->buffers[2];
    if (status == 0)
        status =
            MS_KERNEL(MS_K_RACK_DEFOCUS_KERNEL, api, MS_FLOAT("radius_r", j->radius[0]),
                      MS_FLOAT("radius_g", j->radius[1]), MS_FLOAT("radius_b", j->radius[2]),
                      MS_FLOAT("rel_height", j->rel_height), MS_INT("sides", j->sides),
                      MS_FLOAT("power", j->power), MS_FLOAT("bokeh", j->bokeh),
                      MS_INT("gauss", j->gauss), MS_INT("half", j->half), MS_IMAGE("kernel", lens));
    if (status == 0)
        status = ms_fft(api, &lens, &image, 0);
    // `image` is free again (the scratch of the lens' transform).
    if (status == 0)
        status = MS_KERNEL(MS_K_RACK_DEFOCUS_PREPARE, api, MS_IMAGE("source", j->source),
                           MS_INT("pad_x", j->pad), MS_INT("pad_y", j->pad),
                           MS_INT("edge_mode", j->edge_mode), MS_INT("mono", j->mono),
                           MS_FLOAT("gamma", j->gamma), MS_FLOAT("boost", j->boost),
                           MS_FLOAT("threshold", j->threshold), MS_IMAGE("data", image));
    if (status == 0)
        status = ms_fft(api, &image, &free_image, 0);
    if (status == 0)
        status = MS_KERNEL(MS_K_RACK_DEFOCUS_MULTIPLY, api, MS_IMAGE("image", image),
                           MS_IMAGE("lens", lens), MS_IMAGE("product", free_image));
    if (status == 0)
        status = ms_fft(api, &free_image, &image, 1);
    if (status != 0)
        return status;
    return MS_KERNEL(MS_K_RACK_DEFOCUS_FINISH, api, MS_IMAGE("data", free_image),
                     MS_IMAGE("source", j->source), MS_IMAGE("mask_value", &j->mask->value),
                     MS_INT("pad_x", j->pad), MS_INT("pad_y", j->pad), MS_FLOAT("scale", j->scale),
                     MS_FLOAT("gamma", j->gamma), MS_FLOAT("brightness", j->brightness),
                     MS_FLOAT("offset_darks", j->offset_darks), MS_FLOAT("mix", j->mix),
                     MS_IMAGE("output", j->output));
}

// The Circle: a triangle all but round, fitted to the reference (an
// exact circle is up to 2e-4 further).
#define CIRCLE 0.0035f

// Roundness as the power of the polygon's radius (fitted: 1 at 0, toward 0 at 1, a star
// below 0).
static float power_of(double roundness) {
    return (float)(roundness >= 0 ? pow(1.0 - roundness, 1.7) : 1.0 - 4.6 * roundness);
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height;
    const double width =
        v[DEFOCUS_WIDTH] > 0 ? (v[DEFOCUS_WIDTH] < 1e5 ? v[DEFOCUS_WIDTH] : 1e5) : 0;
    const double gauss = v[GAUSS_BLUR] > 0 ? (v[GAUSS_BLUR] < 1e5 ? v[GAUSS_BLUR] : 1e5) : 0;
    Job j = {.edge_mode = (int)v[EDGE_MODE],
             .mono = (int)v[MODE] == 1,
             .sides = (int)v[SHAPE] == 0 ? 3 : (int)v[SHAPE] + 2,
             .gauss = (int)floor(gauss + 0.5),
             .gamma = (float)v[USE_GAMMA],
             .threshold = (float)v[THRESHOLD],
             .rel_height = (float)v[REL_HEIGHT],
             .power = (int)v[SHAPE] == 0 ? CIRCLE : power_of(v[ROUNDNESS]),
             .bokeh = (float)v[BOKEH],
             .brightness = (float)v[BRIGHTNESS],
             .offset_darks = (float)v[OFFSET_DARKS],
             .mix = (float)v[MIX]};
    const double boost = v[BOOST] * width / r->frame_width;
    j.boost = (float)(64000.0 * boost * boost);
    const double fringe = pow(2.0, v[FRINGING]);
    j.radius[0] = (float)(width / fringe);
    j.radius[1] = (float)width;
    j.radius[2] = (float)(width * fringe);
    // The lens' square, and as much padding around the Source (no wrapping around within).
    const double widest =
        (fringe > 1 ? width * fringe : width / fringe) * (v[REL_HEIGHT] > 1 ? v[REL_HEIGHT] : 1);
    j.half = (int)ceil(widest) + 2 + j.gauss;
    j.pad = j.half;
    const int nx = ms_fft_length(w + 2 * j.pad), ny = ms_fft_length(h + 2 * j.pad);
    j.scale = (float)(1.0 / ((double)nx * ny));

    const size_t size = (size_t)nx * ny * 4;
    float *buffers[3] = {malloc(size * sizeof(float)), malloc(size * sizeof(float)),
                         malloc(size * sizeof(float))};
    MsImage source;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    MsMask mask = {0};
    int ok = buffers[0] && buffers[1] && buffers[2];
    if (!ok)
        snprintf(r->err, r->err_size, "not enough memory");
    else
        ok = ms_mask_init(&mask, r, MASK, SOURCE, &source, v[MASK_USE], v[INVERT_MASK],
                          v[BLUR_MASK]);
    if (ok) {
        MsImage output, images[3];
        ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
        for (int i = 0; i < 3; ++i) {
            ms_image(&images[i], buffers[i], (ptrdiff_t)nx * 4, nx, ny, 4);
            j.buffers[i] = &images[i];
        }
        j.source = &source;
        j.mask = &mask;
        j.output = &output;
        // Inputs uploaded to the GPU: the Source, and the Mask or the one mask value.
        MsImage *const inputs[] = {&source, mask.input};
        MsImage *between[4] = {&images[0], &images[1], &images[2]};
        int between_count = 3;
        if (mask.has_mask)
            between[between_count++] = &mask.value;
        ok = ms_run(run, &j, inputs, 2, between, between_count, &output, r->info, r->err,
                    r->err_size);
    }
    ms_mask_free(&mask);
    for (int i = 0; i < 3; ++i)
        free(buffers[i]);
    return ok;
}

// No lens and no change of the colors, or Mix With Source at 1: the Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    const int no_lens = v[DEFOCUS_WIDTH] <= 0 && floor(v[GAUSS_BLUR] + 0.5) <= 0;
    return v[MIX] >= 1.0 ||
           (no_lens && (int)v[MODE] == 0 && v[BRIGHTNESS] == 1.0 && v[OFFSET_DARKS] == 0.0);
}

const MsEffect ms_rackdefocus_effect = {
    MS_ID("RackDefocus"),
    "MS_RackDefocus",
    "Blurs the image as an out-of-focus lens would, by its shape.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
    identity,
    0,
    NULL,
    NULL,
    0,
};
