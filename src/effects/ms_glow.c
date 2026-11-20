// MS_Glow: glows from the bright parts of the image (src/halide/glow_generator.cpp).
//   - the threshold: the Source (towards its alpha by Glow From Alpha, times the Mask's RGB),
//     each channel above Threshold (+ Threshold Add Color) rescaled to 0..1, the pixel's
//     colour kept
//   - blurred by 3 box blurs per axis, mirrored borders (Edge Mode Reflect) or black ones
//     (Transparent), total radius Glow Width x Width X / Y x Width Red / Green / Blue
//     (Subpixel: between whole radii)
//   - times Brightness x Color, combined with the Source scaled by Source Opacity: Mult, Add,
//     Screen, Difference or Overlay (Screen and Overlay above 1: the larger); Glow Under
//     Source moves it towards the Source over the glow; the alpha is combined the same way
//     with the glow's largest channel times Affect Alpha
// Glow Width is a "normalised" value, so hosts show it in pixels.

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

static const char *const show_entries[] = {"Result", "Threshold"};
static const char *const combine_entries[] = {"Mult", "Add", "Screen", "Difference", "Overlay"};
static const char *const edge_entries[] = {"Transparent", "Reflect"};
enum { MULT, ADD, SCREEN, DIFFERENCE, OVERLAY };

enum {
    BRIGHTNESS,
    COLOR,
    COLOR_G,
    COLOR_B,
    THRESHOLD,
    ADD_COLOR,
    ADD_COLOR_G,
    ADD_COLOR_B,
    WIDTH,
    WIDTH_X,
    WIDTH_Y,
    WIDTH_RED,
    WIDTH_GREEN,
    WIDTH_BLUE,
    SUBPIXEL,
    SHOW,
    AFFECT_ALPHA,
    FROM_ALPHA,
    UNDER_SOURCE,
    SOURCE_OPACITY,
    COMBINE,
    EDGE_MODE,
    INVERT_MASK,
    SHOW_GLOW_WIDTH,
};
static const MsParam params[] = {
    [BRIGHTNESS] = {.type = MS_PARAM_DOUBLE,
                    .name = "brightness",
                    .label = "Brightness",
                    .hint = "Scales the brightness of all the glows.",
                    .def = 2.0,
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 5.0},
    [COLOR] = {.type = MS_PARAM_RGB,
               .name = "color",
               .label = "Color",
               .hint = "Scales the color of the glows.",
               .def = 1.0},
    [COLOR_G] = {.type = MS_PARAM_RGB_PART, .name = "color_green", .def = 1.0},
    [COLOR_B] = {.type = MS_PARAM_RGB_PART, .name = "color_blue", .def = 1.0},
    [THRESHOLD] = {.type = MS_PARAM_DOUBLE,
                   .name = "threshold",
                   .label = "Threshold",
                   .hint = "Glows come from the parts of the image brighter than this.",
                   .def = 0.5,
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 1.0},
    [ADD_COLOR] = {.type = MS_PARAM_RGB,
                   .name = "threshold_add_color",
                   .label = "Threshold Add Color",
                   .hint = "Raises the threshold of each color channel by this."},
    [ADD_COLOR_G] = {.type = MS_PARAM_RGB_PART, .name = "threshold_add_color_green"},
    [ADD_COLOR_B] = {.type = MS_PARAM_RGB_PART, .name = "threshold_add_color_blue"},
    [WIDTH] = {.type = MS_PARAM_WIDTH,
               .name = "glow_width",
               .label = "Glow Width",
               .hint = "The size of the glows.",
               .def = 0.05,
               .min = 0.0,
               .max = MS_UNBOUNDED,
               .display_max = 0.5},
    MS_RELATIVE(WIDTH_X, "width_x", "Width X", "Scales the horizontal glow width.", 1.0),
    MS_RELATIVE(WIDTH_Y, "width_y", "Width Y", "Scales the vertical glow width.", 1.0),
    MS_RELATIVE(WIDTH_RED, "width_red", "Width Red", "Scales the red glow width.", 1.0),
    MS_RELATIVE(WIDTH_GREEN, "width_green", "Width Green", "Scales the green glow width.", 1.2),
    MS_RELATIVE(WIDTH_BLUE, "width_blue", "Width Blue", "Scales the blue glow width.", 1.4),
    [SUBPIXEL] = {.type = MS_PARAM_BOOL,
                  .name = "subpixel",
                  .label = "Subpixel",
                  .hint = "Glow widths between whole pixels, for smooth animation.",
                  .def = 1.0},
    [SHOW] = {.type = MS_PARAM_CHOICE,
              .name = "show",
              .label = "Show",
              .hint = "The result, or the thresholded image the glows come from.",
              .entries = show_entries,
              .entry_count = ARRAY_SIZE(show_entries)},
    [AFFECT_ALPHA] = {.type = MS_PARAM_DOUBLE,
                      .name = "affect_alpha",
                      .label = "Affect Alpha",
                      .hint = "Adds the glows to the alpha, scaled by this.",
                      .min = 0.0,
                      .max = MS_UNBOUNDED,
                      .display_max = 1.0},
    [FROM_ALPHA] = {.type = MS_PARAM_DOUBLE,
                    .name = "glow_from_alpha",
                    .label = "Glow From Alpha",
                    .hint = "1: the glows come from the alpha instead of the colors.",
                    .min = 0.0,
                    .max = 1.0,
                    .display_max = 1.0},
    [UNDER_SOURCE] = {.type = MS_PARAM_DOUBLE,
                      .name = "glow_under_source",
                      .label = "Glow Under Source",
                      .hint = "1: the Source over the glows.",
                      .min = 0.0,
                      .max = 1.0,
                      .display_max = 1.0},
    [SOURCE_OPACITY] = {.type = MS_PARAM_DOUBLE,
                        .name = "source_opacity",
                        .label = "Source Opacity",
                        .hint = "Scales the Source combined with the glows.",
                        .def = 1.0,
                        .min = 0.0,
                        .max = 1.0,
                        .display_max = 1.0},
    [COMBINE] = {.type = MS_PARAM_CHOICE,
                 .name = "combine",
                 .label = "Combine",
                 .hint = "How the glows are combined with the Source.",
                 .def = SCREEN,
                 .entries = combine_entries,
                 .entry_count = ARRAY_SIZE(combine_entries)},
    [EDGE_MODE] = {.type = MS_PARAM_CHOICE,
                   .name = "edge_mode",
                   .label = "Edge Mode",
                   .hint = "Outside the image: black (Transparent) or the image mirrored.",
                   .def = 1,
                   .entries = edge_entries,
                   .entry_count = ARRAY_SIZE(edge_entries)},
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
    MS_SHOW(SHOW_GLOW_WIDTH, "ShowGlowWidth", "Show Glow Width"),
};

// One render: images and settings of the kernels (src/halide/glow_generator.cpp).
typedef struct {
    MsImage *source, *mask, *glow, *output;
    int has_mask, invert_mask, transparent, combine, show_threshold;
    int r[2][3];   // total radius per axis (x, y) and channel: whole pixels
    float f[2][3]; // and the fraction past them
    float threshold, add[3], from_alpha, gain[3], source_opacity, under, affect_alpha;
} Job;

// The glow (unless Show is Threshold), then the result, on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    if (!j->show_threshold) {
        const int status = MS_KERNEL(
            MS_K_GLOW_BLUR, api, MS_IMAGE("source", j->source), MS_IMAGE("mask", j->mask),
            MS_INT("has_mask", j->has_mask), MS_INT("invert_mask", j->invert_mask),
            MS_FLOAT("threshold", j->threshold), MS_FLOAT("add_r", j->add[0]),
            MS_FLOAT("add_g", j->add[1]), MS_FLOAT("add_b", j->add[2]),
            MS_FLOAT("from_alpha", j->from_alpha), MS_INT("rx_r", j->r[0][0]),
            MS_INT("rx_g", j->r[0][1]), MS_INT("rx_b", j->r[0][2]), MS_INT("ry_r", j->r[1][0]),
            MS_INT("ry_g", j->r[1][1]), MS_INT("ry_b", j->r[1][2]), MS_FLOAT("fx_r", j->f[0][0]),
            MS_FLOAT("fx_g", j->f[0][1]), MS_FLOAT("fx_b", j->f[0][2]),
            MS_FLOAT("fy_r", j->f[1][0]), MS_FLOAT("fy_g", j->f[1][1]),
            MS_FLOAT("fy_b", j->f[1][2]), MS_INT("transparent", j->transparent),
            MS_IMAGE("glow", j->glow));
        if (status != 0)
            return status;
    }
    return MS_KERNEL(MS_K_GLOW_COMBINE, api, MS_IMAGE("source", j->source),
                     MS_IMAGE("glow", j->show_threshold ? j->source : j->glow),
                     MS_IMAGE("mask", j->mask), MS_INT("has_mask", j->has_mask),
                     MS_INT("invert_mask", j->invert_mask), MS_FLOAT("threshold", j->threshold),
                     MS_FLOAT("add_r", j->add[0]), MS_FLOAT("add_g", j->add[1]),
                     MS_FLOAT("add_b", j->add[2]), MS_FLOAT("from_alpha", j->from_alpha),
                     MS_FLOAT("gain_r", j->gain[0]), MS_FLOAT("gain_g", j->gain[1]),
                     MS_FLOAT("gain_b", j->gain[2]), MS_INT("combine", j->combine),
                     MS_FLOAT("source_opacity", j->source_opacity), MS_FLOAT("under", j->under),
                     MS_FLOAT("affect_alpha", j->affect_alpha),
                     MS_INT("show_threshold", j->show_threshold), MS_IMAGE("output", j->output));
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height, has_mask = r->connected[MASK];
    const int show_threshold = (int)v[SHOW] == 1;
    // The glow, between the two kernels.
    float *glow = show_threshold ? NULL : malloc((size_t)w * h * 4 * sizeof(float));
    if (!show_threshold && !glow) {
        snprintf(r->err, r->err_size, "not enough memory");
        return 0;
    }
    MsImage source, mask, glow_image, output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&mask, r->inputs[MASK].pixels, r->inputs[MASK].row, r->half, w, h);
    ms_image(&glow_image, glow, (ptrdiff_t)w * 4, w, h, 4);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    // Without a Mask, or one that is the Source's image in the host: the Source stands in
    // (uploaded once).
    const int mask_is_source = has_mask && ms_same_input(&r->inputs[MASK], &r->inputs[SOURCE]);
    Job j = {.source = &source,
             .mask = has_mask && !mask_is_source ? &mask : &source,
             .glow = &glow_image,
             .output = &output,
             .has_mask = has_mask,
             .invert_mask = (int)v[INVERT_MASK],
             .transparent = (int)v[EDGE_MODE] == 0,
             .combine = (int)v[COMBINE],
             .show_threshold = show_threshold,
             .threshold = (float)v[THRESHOLD],
             .from_alpha = (float)v[FROM_ALPHA],
             .source_opacity = (float)v[SOURCE_OPACITY],
             .under = (float)v[UNDER_SOURCE],
             .affect_alpha = (float)v[AFFECT_ALPHA]};
    // Total radii: Subpixel keeps the fraction, else rounded.
    const double axis[2] = {v[WIDTH_X], v[WIDTH_Y]};
    const double channel[3] = {v[WIDTH_RED], v[WIDTH_GREEN], v[WIDTH_BLUE]};
    for (int a = 0; a < 2; ++a)
        for (int c = 0; c < 3; ++c) {
            double t = v[WIDTH] * axis[a] * channel[c];
            t = t > 0 ? (t < 1e7 ? t : 1e7) : 0;
            const double whole = v[SUBPIXEL] != 0 ? floor(t) : floor(t + 0.5);
            j.r[a][c] = (int)whole;
            j.f[a][c] = (float)(v[SUBPIXEL] != 0 ? t - whole : 0.0);
        }
    for (int c = 0; c < 3; ++c) {
        j.add[c] = (float)v[ADD_COLOR + c];
        j.gain[c] = (float)(v[BRIGHTNESS] * v[COLOR + c]);
    }
    // Inputs uploaded to the GPU: the Source, and the Mask.
    MsImage *const inputs[] = {&source, j.mask};
    MsImage *const between[] = {&glow_image};
    const int ok = ms_run(run, &j, inputs, 2, between, show_threshold ? 0 : 1, &output, r->info,
                          r->err, r->err_size);
    free(glow);
    return ok;
}

// No glow (Brightness or Color 0) combined so that it leaves the Source (and its alpha) as
// it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    const int combine = (int)v[COMBINE];
    const int no_glow =
        v[BRIGHTNESS] == 0.0 || (v[COLOR] == 0.0 && v[COLOR_G] == 0.0 && v[COLOR_B] == 0.0);
    return no_glow && (int)v[SHOW] == 0 && v[SOURCE_OPACITY] == 1.0 &&
           (combine == ADD || combine == SCREEN || combine == DIFFERENCE);
}

// The viewer: the Glow Width (white, 0.9 x Glow Width), the colours' widths, Width X and Y
// (the arrows).
static const MsOverlay overlay[] = {
    {.kind = MS_OVERLAY_ELLIPSE,
     .colour = MS_OVERLAY_WHITE,
     .show = "ShowGlowWidth",
     .size = "glow_width",
     .rel_x = "width_x",
     .rel_y = "width_y",
     .factor = 0.9},
    {.kind = MS_OVERLAY_ELLIPSE,
     .colour = MS_OVERLAY_RED,
     .show = "ShowGlowWidth",
     .size = "glow_width",
     .scale = "width_red",
     .rel_x = "width_x",
     .rel_y = "width_y"},
    {.kind = MS_OVERLAY_ELLIPSE,
     .colour = MS_OVERLAY_GREEN,
     .show = "ShowGlowWidth",
     .size = "glow_width",
     .scale = "width_green",
     .rel_x = "width_x",
     .rel_y = "width_y"},
    {.kind = MS_OVERLAY_ELLIPSE,
     .colour = MS_OVERLAY_BLUE,
     .show = "ShowGlowWidth",
     .size = "glow_width",
     .scale = "width_blue",
     .rel_x = "width_x",
     .rel_y = "width_y"},
    {.kind = MS_OVERLAY_ARROW_X,
     .colour = MS_OVERLAY_ORANGE,
     .show = "ShowGlowWidth",
     .size = "glow_width",
     .rel_x = "width_x",
     .rel_y = "width_y",
     .factor = 0.9},
    {.kind = MS_OVERLAY_ARROW_Y,
     .colour = MS_OVERLAY_ORANGE,
     .show = "ShowGlowWidth",
     .size = "glow_width",
     .rel_x = "width_x",
     .rel_y = "width_y",
     .factor = 0.9},
};

const MsEffect ms_glow_effect = {
    MS_ID("Glow"),
    "MS_Glow",
    "Glows from the parts of the image brighter than a threshold.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
    identity,
    0,
    NULL,
    overlay,
    ARRAY_SIZE(overlay),
};
