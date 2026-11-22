// MS_RackDefocus: a lens blur (src/halide/rack_defocus_generator.cpp), by FFT (fft.c).
//   - the Source padded by the Edge Mode (Reflect: mirrored without repeating the border
//     pixel), Defocus Mono: its luma (5 R + 8 G + 3 B) / 16; Use Gamma g: v^g between 0 and
//     1; Boost Highlights b: v (1 + c max(m - Hilight Threshold, 0)), m the largest channel, with
//     c = 64000 (b x Defocus Width / frame width)^2, on the colours
//   - convolved by the lens: the Shape (a regular polygon, a vertex up, Roundness bending its
//     sides; the Circle a rounded triangle) of radius Defocus Width (x Rel Height in y), each
//     pixel's coverage, weighted by Bokeh (x = (d / radius)^2: 1 - b x, or 1 + b - b x below 0),
//     its radius per channel R 2^-f, R, R 2^f by Color Fringing f, summing to 1
//   - Gauss Blur: the padded Source blurred (blur.h's 3 boxes) before Use Gamma and Boost
//     Highlights
//   - Chroma Distort: before the blur, each channel a mix of the Source zoomed about the frame's
//     middle by five factors (1 +- about 2 ln 2 x Chroma Distort x R / frame width)
//   - Rotate turns the lens (counterclockwise, before Rel Height); Lens Noise: each pixel x
//     max(0, 1 - Lens Noise x max(0, n)), n a noise in the lens' frame (MillSaph's), Noise Freq
//     cycles across its radius (Noise Freq Rel X: no effect, as the reference's)
//   - inverse gamma, Brightness x b + Offset Darks x (1 - b) on the colours, then Mix With
//     Source and the Mask
//   - Show Shape: the lens alone in the middle of the image, at most 1
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
    SHOW_SHAPE,
    ROUNDNESS,
    ROTATE,
    BOKEH,
    LENS_NOISE,
    NOISE_FREQ,
    NOISE_FREQ_REL_X,
    NOISE_SEED,
    GAUSS_BLUR,
    USE_GAMMA,
    BOOST,
    THRESHOLD,
    CHROMA_DISTORT,
    FRINGING,
    BRIGHTNESS,
    OFFSET_DARKS,
    MIX,
    EDGE_MODE,
    MASK_USE,
    BLUR_MASK,
    INVERT_MASK,
    SHOW_DEFOCUS_WIDTH,
};
static const MsParam params[] = {
    [MODE] = {.type = MS_PARAM_CHOICE,
              .name = "mode",
              .label = "Mode",
              .hint = "Blurs the colors, or the luma alone (a monochrome result).",
              .entries = mode_entries,
              .entry_count = ARRAY_SIZE(mode_entries)},
    MS_WIDTH(DEFOCUS_WIDTH, "defocus_width", "Defocus Width", "The radius of the lens.", 0.011,
             0.25),
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
    [SHOW_SHAPE] = {.type = MS_PARAM_BOOL,
                    .name = "show_shape",
                    .label = "Show Shape",
                    .hint = "Shows the lens itself, in the middle of the image."},
    [ROUNDNESS] = {.type = MS_PARAM_DOUBLE,
                   .name = "roundness",
                   .label = "Roundness",
                   .hint = "Bends the polygon's sides: out toward a circle, or in toward a star.",
                   .min = -1.0,
                   .max = 1.0,
                   .display_min = -1.0,
                   .display_max = 1.0},
    [ROTATE] = {.type = MS_PARAM_ANGLE,
                .name = "rotate",
                .label = "Rotate",
                .hint = "Turns the lens (degrees, counterclockwise).",
                .display_min = -360.0,
                .display_max = 360.0},
    [BOKEH] = {.type = MS_PARAM_DOUBLE,
               .name = "bokeh",
               .label = "Bokeh",
               .hint = "Brighter lens center (above 0) or rim (below 0).",
               .min = -1.0,
               .max = 1.0,
               .display_min = -1.0,
               .display_max = 1.0},
    MS_RELATIVE(LENS_NOISE, "lens_noise", "Lens Noise", "Dark specks across the lens (0: none).",
                0.0),
    [NOISE_FREQ] = {.type = MS_PARAM_DOUBLE,
                    .name = "noise_freq",
                    .label = "Noise Freq",
                    .hint = "How fine the Lens Noise is: its cycles across the lens' radius.",
                    .def = 40.0,
                    .min = 0.01,
                    .max = MS_UNBOUNDED,
                    .display_min = 0.1,
                    .display_max = 50.0},
    [NOISE_FREQ_REL_X] = {.type = MS_PARAM_DOUBLE,
                          .name = "noise_freq_rel_x",
                          .label = "Noise Freq Rel X",
                          .hint = "No effect (kept for the presets that set it).",
                          .def = 1.0,
                          .min = 0.01,
                          .max = MS_UNBOUNDED,
                          .display_min = 0.1,
                          .display_max = 10.0},
    [NOISE_SEED] = {.type = MS_PARAM_DOUBLE,
                    .name = "noise_seed",
                    .label = "Noise Seed",
                    .hint = "Another Lens Noise for each value.",
                    .def = 0.123,
                    .display_max = 5.0},
    MS_WIDTH(GAUSS_BLUR, "gauss_blur", "Gauss Blur", "Softens the lens' edge by this much.", 0.0,
             0.05),
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
    MS_RELATIVE(BOOST, "boost_highlights", "Boost Highlights",
                "Brightens the parts above Hilight Threshold before the blur.", 0.0),
    [THRESHOLD] = {.type = MS_PARAM_DOUBLE,
                   .name = "hilight_threshold",
                   .label = "Hilight Threshold",
                   .hint = "Boost Highlights brightens the parts brighter than this.",
                   .def = 0.9,
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 1.0},
    [CHROMA_DISTORT] = {.type = MS_PARAM_DOUBLE,
                        .name = "chroma_distort",
                        .label = "Chroma Distort",
                        .hint = "Splits the colors toward the frame's edges, more with a wider "
                                "lens: red inward and blue outward (or the reverse below 0).",
                        .min = -5.0,
                        .max = 5.0,
                        .display_min = -0.5,
                        .display_max = 0.5},
    [FRINGING] = {.type = MS_PARAM_DOUBLE,
                  .name = "color_fringing",
                  .label = "Color Fringing",
                  .hint = "A wider lens for blue and a narrower one for red (or the reverse "
                          "below 0).",
                  .min = -5.0,
                  .max = 5.0,
                  .display_min = -0.5,
                  .display_max = 0.5},
    MS_RELATIVE(BRIGHTNESS, "brightness", "Brightness", "Scales the blurred colors.", 1.0),
    [OFFSET_DARKS] = {.type = MS_PARAM_DOUBLE,
                      .name = "offset_darks",
                      .label = "Offset Darks",
                      .hint = "Raises the dark parts of the blurred colors toward 1 (or lowers "
                              "them below 0).",
                      .min = -MS_UNBOUNDED,
                      .max = MS_UNBOUNDED,
                      .display_min = -5.0,
                      .display_max = 1.0},
    MS_FRACTION(MIX, "mix_with_source", "Mix With Source",
                "Mixes the Source back over the result: 0 for the blur only.", 0.0),
    [EDGE_MODE] = {.type = MS_PARAM_CHOICE,
                   .name = "edge_mode",
                   .label = "Edge Mode",
                   .hint = "Outside the image: black, the border pixels repeated, or the image "
                           "mirrored.",
                   .def = 2,
                   .entries = edge_entries,
                   .entry_count = ARRAY_SIZE(edge_entries)},
    [MASK_USE] = MS_PARAM_MASK_USE,
    [BLUR_MASK] = MS_PARAM_BLUR_MASK(0.00625, 0.5),
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
    MS_SHOW(SHOW_DEFOCUS_WIDTH, "ShowDefocusWidth", "Show Defocus Width"),
};

// One render: images and settings of the kernels.
typedef struct {
    MsImage *source, *output;
    MsMask *mask;
    MsImage *buffers[3]; // the transforms' images (width x height)
    int pad, edge_mode, mono, sides, gauss, half, show, cx, cy, noise_octaves, zoom;
    float zooms[5]; // Chroma Distort's zoom factors
    float gamma, boost, threshold, radius[3], rel_height, power, bokeh;
    float turn_cos, turn_sin, noise_amp, noise_freq, noise_z;
    float brightness, offset_darks, mix, scale;
} Job;

// The mask value (with a Mask); the lens and the Source, transformed; their product,
// transformed back; the result, on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    int status = ms_mask_run(j->mask, api);
    MsImage *lens = j->buffers[0], *image = j->buffers[1], *free_image = j->buffers[2];
    if (status == 0)
        status = MS_KERNEL(MS_K_RACK_DEFOCUS_KERNEL, api, MS_FLOAT("radius_r", j->radius[0]),
                           MS_FLOAT("radius_g", j->radius[1]), MS_FLOAT("radius_b", j->radius[2]),
                           MS_FLOAT("rel_height", j->rel_height), MS_INT("sides", j->sides),
                           MS_FLOAT("power", j->power), MS_FLOAT("bokeh", j->bokeh),
                           MS_INT("gauss", j->show ? j->gauss : 0), MS_INT("half", j->half),
                           MS_FLOAT("turn_cos", j->turn_cos), MS_FLOAT("turn_sin", j->turn_sin),
                           MS_FLOAT("noise_amp", j->noise_amp),
                           MS_FLOAT("noise_freq", j->noise_freq), MS_FLOAT("noise_z", j->noise_z),
                           MS_INT("noise_octaves", j->noise_octaves), MS_IMAGE("kernel", lens));
    if (status == 0 && j->show) // Show Shape: the lens alone
        return MS_KERNEL(MS_K_RACK_DEFOCUS_SHOW, api, MS_IMAGE("lens", lens), MS_INT("cx", j->cx),
                         MS_INT("cy", j->cy), MS_IMAGE("output", j->output));
    if (status == 0)
        status = ms_fft(api, &lens, &image, 0);
    // `image` is free again (the scratch of the lens' transform).
    if (status == 0)
        status = MS_KERNEL(MS_K_RACK_DEFOCUS_PREPARE, api, MS_IMAGE("source", j->source),
                           MS_INT("pad_x", j->pad), MS_INT("pad_y", j->pad),
                           MS_INT("edge_mode", j->edge_mode), MS_INT("mono", j->mono),
                           MS_FLOAT("gamma", j->gamma), MS_FLOAT("boost", j->boost),
                           MS_FLOAT("threshold", j->threshold), MS_INT("zoom", j->zoom),
                           MS_FLOAT("zoom0", j->zooms[0]), MS_FLOAT("zoom1", j->zooms[1]),
                           MS_FLOAT("zoom2", j->zooms[2]), MS_FLOAT("zoom3", j->zooms[3]),
                           MS_FLOAT("zoom4", j->zooms[4]), MS_INT("tone", j->gauss == 0),
                           MS_IMAGE("data", j->gauss ? free_image : image));
    // Gauss Blur: on the prepared Source, before Use Gamma and Boost Highlights.
    if (status == 0 && j->gauss)
        status = MS_KERNEL(MS_K_RACK_DEFOCUS_SOFTEN, api, MS_IMAGE("prepared", free_image),
                           MS_INT("gauss", j->gauss), MS_FLOAT("gamma", j->gamma),
                           MS_FLOAT("boost", j->boost), MS_FLOAT("threshold", j->threshold),
                           MS_IMAGE("data", image));
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

// Lens Noise: an fbm (persistence 0.5) of standard deviation about 0.125 x Lens Noise, Noise
// Freq cycles across the lens' radius, octaves down to 2 pixels; finer than 2 pixels, weaker as
// the pixels average it (1 / sqrt(1 + (f / 1.2)^2), f in cycles per pixel: the reference's
// 0.0875 at 1.3). 0.28: one octave's standard deviation (noise.h).
static void lens_noise(const double *v, double radius, Job *j) {
    const double f = v[NOISE_FREQ] / fmax(radius, 1e-6); // cycles per pixel
    int octaves = (int)floor(log2(fmax(0.5 / f, 1.0))) + 1;
    octaves = octaves < 8 ? octaves : 8;
    double var = 0.0;
    for (int k = 0; k < octaves; ++k)
        var += pow(0.25, k);
    const double sd = 0.28 * sqrt(var);
    j->noise_amp = (float)(v[LENS_NOISE] * 0.125 / sqrt(1.0 + (f / 1.2) * (f / 1.2)) / sd);
    j->noise_freq = (float)f;
    j->noise_z = (float)fmod(37.1 * v[NOISE_SEED], 4096.0);
    j->noise_octaves = octaves;
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
             .show = v[SHOW_SHAPE] != 0,
             .cx = w / 2,
             .cy = h / 2,
             .turn_cos = (float)cos(v[ROTATE] * M_PI / 180.0),
             .turn_sin = (float)sin(v[ROTATE] * M_PI / 180.0),
             .brightness = (float)v[BRIGHTNESS],
             .offset_darks = (float)v[OFFSET_DARKS],
             .mix = (float)v[MIX]};
    const double boost = v[BOOST] * width / r->frame_width;
    j.boost = (float)(64000.0 * boost * boost);
    const double fringe = pow(2.0, v[FRINGING]);
    j.radius[0] = (float)(width / fringe);
    j.radius[1] = (float)width;
    j.radius[2] = (float)(width * fringe);
    lens_noise(v, width, &j);
    // Chroma Distort d: five zooms 1 + (2 R / frame width) (ln 2 d k + 0.2404 d^2 + 0.0134 d^3 k),
    // k = -2..2 (fitted, R the Defocus Width), none in Defocus Mono.
    const double d = v[CHROMA_DISTORT], rho = 2.0 * width / r->frame_width;
    j.zoom = d != 0.0 && width > 0 && !j.mono;
    for (int k = 0; k < 5; ++k)
        j.zooms[k] = (float)(1.0 + rho * (M_LN2 * d * (k - 2) + 0.2404 * d * d +
                                          0.0134 * d * d * d * (k - 2)));
    // The lens' square, and as much padding around the Source (no wrapping around within).
    const double widest =
        (fringe > 1 ? width * fringe : width / fringe) * (v[REL_HEIGHT] > 1 ? v[REL_HEIGHT] : 1);
    j.half = (int)ceil(widest) + 2 + j.gauss;
    j.pad = j.half;
    const int nx = ms_fft_length(w + 2 * j.pad), ny = ms_fft_length(h + 2 * j.pad);
    j.scale = (float)(1.0 / ((double)nx * ny));

    // The three transforms' buffers and the mask value.
    const size_t size = (size_t)nx * ny * 4;
    const size_t floats[] = {size, size, size, ms_mask_floats(r, MASK)};
    float *block = malloc(ms_parts_size(floats, ARRAY_SIZE(floats))), *part[ARRAY_SIZE(floats)];
    if (!block)
        return ms_out_of_memory(r);
    ms_parts(block, floats, ARRAY_SIZE(floats), part);
    MsImage source, output, images[3];
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    for (int i = 0; i < 3; ++i) {
        ms_image(&images[i], part[i], (ptrdiff_t)nx * 4, nx, ny, 4);
        j.buffers[i] = &images[i];
    }
    MsMask mask;
    ms_mask_init(&mask, r, MASK, SOURCE, &source, part[3], v[MASK_USE], v[INVERT_MASK],
                 v[BLUR_MASK]);
    j.source = &source;
    j.mask = &mask;
    j.output = &output;
    // Inputs uploaded to the GPU: the Source, and the Mask or the one mask value.
    MsImage *const inputs[] = {&source, mask.input};
    MsImage *const between[] = {&images[0], &images[1], &images[2], ms_mask_between(&mask)};
    const int ok = ms_run(run, &j, inputs, ARRAY_SIZE(inputs), between, ARRAY_SIZE(between),
                          &output, r->info, r->err, r->err_size);
    free(block);
    return ok;
}

// No lens and no change of the colors, or Mix With Source at 1: the Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    const int no_lens = v[DEFOCUS_WIDTH] <= 0 && floor(v[GAUSS_BLUR] + 0.5) <= 0;
    return v[MIX] >= 1.0 ||
           (no_lens && (int)v[MODE] == 0 && v[BRIGHTNESS] == 1.0 && v[OFFSET_DARKS] == 0.0);
}

// The viewer: the Defocus Width and the Rel Height (the arrow).
static const MsOverlay overlay[] = {
    MS_OVERLAY_SIZED(MS_OVERLAY_ELLIPSE, "ShowDefocusWidth", "defocus_width", NULL, NULL,
                     "rel_height", 0.0, MS_OVERLAY_ORANGE),
    MS_OVERLAY_SIZED(MS_OVERLAY_ARROW_Y, "ShowDefocusWidth", "defocus_width", NULL, NULL,
                     "rel_height", 0.0, MS_OVERLAY_ORANGE),
};

const MsEffect ms_rackdefocus_effect = {
    .id = MS_ID("RackDefocus"),
    .label = "MS_RackDefocus",
    .description = "Blurs the image as an out-of-focus lens would, by its shape.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    .identity = identity,
    MS_OVERLAYS(overlay),
    .presets = "rackdefocus",
};
