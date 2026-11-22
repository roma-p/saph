// MS_Glare: a glare (a style of elements: glows, rays, sectors, rings) on each bright spot of
// the image (src/halide/glare_generator.cpp).
//   - the spots: the Source (towards its alpha by Glare From Alpha, times the Mask), blurred
//     by Threshold Blur, above Threshold (Glint's ms_glint_source); each connected region
//     one spot, of strength min(its largest channel x Brightness x Scale Colors, Size^2 /
//     100), its colour kept, at its centroid
//   - the style (a JSON text, glare_style.c): its elements, sized by Size (and Rel Height,
//     Rotate, the Rays settings), drawn once around one spot of strength (frame width /
//     10)^2 / 100, which gives each element its brightness at its peak
//   - the glare: the spots convolved with that drawing (by FFT, effects/fft.c), times
//     Brightness x Scale Colors again; Hue Shift, Saturation, Blur Glare; screened over the
//     Source x Source Opacity (or under it), Affect Alpha (as MS_Glint)
// Size, Threshold Blur and Blur Glare are "normalised" values, so hosts show them in pixels.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/fft.h"
#include "effects/glare_style.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
};
enum { SOURCE, MASK };

static const char *const res_entries[] = {"Full", "Half"};
static const char *const mask_type_entries[] = {"Luma", "Color", "Alpha"};

// MillSaph's default style: a core, a soft halo, two stars of rays and a faint ring.
static const char default_style[] =
    "{\"elements\": [\n"
    " {\"type\": \"blob\", \"name\": \"Core\", \"size\": 0.2, \"brightness\": 0.6,"
    " \"colors\": [1, 0.96, 0.9]},\n"
    " {\"type\": \"blob\", \"name\": \"Halo\", \"size\": 0.7, \"brightness\": 0.15,"
    " \"colors\": [0.85, 0.9, 1]},\n"
    " {\"type\": \"rays\", \"name\": \"Star\", \"size\": 0.65, \"brightness\": 0.2,"
    " \"ray_number\": 4, \"ray_width\": 0.02, \"rotate\": 45},\n"
    " {\"type\": \"rays\", \"name\": \"Cross\", \"size\": 0.3, \"brightness\": 0.15,"
    " \"ray_number\": 4, \"ray_width\": 0.08},\n"
    " {\"type\": \"ring\", \"name\": \"Ring\", \"size\": 0.35, \"brightness\": 0.05,"
    " \"radii\": [0.9, 1], \"colors\": [0.8, 0.9, 1, 0.8, 0.9, 1]}\n"
    "]}\n";

enum {
    STYLE,
    BRIGHTNESS,
    SCALE_COLORS,
    SCALE_COLORS_G,
    SCALE_COLORS_B,
    SATURATION,
    THRESHOLD,
    ADD_COLOR,
    ADD_COLOR_G,
    ADD_COLOR_B,
    THRESHOLD_BLUR,
    SIZE,
    REL_HEIGHT,
    ROTATE,
    RAYS_NUM_SCALE,
    RAYS_LENGTH,
    RAYS_THICKNESS,
    BLUR_GLARE,
    HUE_SHIFT,
    GLARE_RES,
    AFFECT_ALPHA,
    FROM_ALPHA,
    UNDER_SOURCE,
    SOURCE_OPACITY,
    INVERT_MASK,
    MASK_TYPE,
    SHOW_SIZE,
};
static const MsParam params[] = {
    [STYLE] = {.type = MS_PARAM_STRING,
               .name = "style_",
               .label = "Style",
               .hint = "The glare's elements, as JSON text: {\"elements\": [...]}, each a blob, "
                       "spot, ring, rays, polar_flare or fancy_spot (containers group them).",
               .def_text = default_style},
    MS_RELATIVE(BRIGHTNESS, "brightness", "Brightness", "Scales the brightness of the glares.",
                1.0),
    MS_COLOUR(SCALE_COLORS, "scale_colors", "Scale Colors", "Scales the color of the glares."),
    MS_RELATIVE(SATURATION, "saturation", "Saturation", "The glares' saturation (0: grey).", 1.0),
    [THRESHOLD] = {.type = MS_PARAM_DOUBLE,
                   .name = "threshold",
                   .label = "Threshold",
                   .hint = "Glares come from the parts of the image brighter than this.",
                   .def = 0.8,
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 1.0},
    MS_COLOUR_OF(ADD_COLOR, "threshold_add_color", "Threshold Add Color",
                 "Raises the threshold of each color channel by this.", 0.0),
    MS_WIDTH(THRESHOLD_BLUR, "threshold_blur", "Threshold Blur",
             "Blurs the image before the threshold: fewer spots from fine "
             "details.",
             0.0028, 0.1),
    MS_WIDTH(SIZE, "size", "Size", "The size of the glares.", 0.1, 0.5),
    MS_RELATIVE(REL_HEIGHT, "rel_height", "Rel Height",
                "The glares' height relative to their width.", 1.0),
    [ROTATE] = {.type = MS_PARAM_ANGLE,
                .name = "rotate",
                .label = "Rotate",
                .hint = "Turns the glares (degrees, counter-clockwise).",
                .min = -MS_UNBOUNDED,
                .max = MS_UNBOUNDED,
                .display_min = -180.0,
                .display_max = 180.0},
    MS_RELATIVE(RAYS_NUM_SCALE, "rays_num_scale", "Rays Num Scale", "Scales the number of rays.",
                1.0),
    MS_RELATIVE(RAYS_LENGTH, "rays_length", "Rays Length", "Scales the length of the rays.", 1.0),
    MS_RELATIVE(RAYS_THICKNESS, "rays_thickness", "Rays Thickness", "Scales the width of the rays.",
                1.0),
    MS_WIDTH(BLUR_GLARE, "blur_glare", "Blur Glare", "Blurs the glares.", 0.0, 0.1),
    [HUE_SHIFT] = {.type = MS_PARAM_DOUBLE,
                   .name = "hue_shift",
                   .label = "Hue Shift",
                   .hint = "Turns the hue of the glares, in turns of the color wheel.",
                   .min = -MS_UNBOUNDED,
                   .max = MS_UNBOUNDED,
                   .display_min = -1.0,
                   .display_max = 1.0},
    [GLARE_RES] = {.type = MS_PARAM_CHOICE,
                   .name = "glare_res",
                   .label = "Glare Res",
                   .hint = "Computes the glares at full or half resolution (faster).",
                   .entries = res_entries,
                   .entry_count = ARRAY_SIZE(res_entries)},
    MS_FRACTION(AFFECT_ALPHA, "affect_alpha", "Affect Alpha", "Adds the glares to the alpha.", 0.0),
    MS_FRACTION(FROM_ALPHA, "glare_from_alpha", "Glare From Alpha",
                "Takes the glares from the alpha instead of the colors.", 0.0),
    MS_FRACTION(UNDER_SOURCE, "glare_under_source", "Glare Under Source",
                "Puts the glares behind the Source, by its alpha.", 0.0),
    MS_FRACTION(SOURCE_OPACITY, "source_opacity", "Source Opacity",
                "Scales the Source under the glares.", 1.0),
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Use the black areas of the Mask."),
    [MASK_TYPE] = {.type = MS_PARAM_CHOICE,
                   .name = "mask_type",
                   .label = "Mask Type",
                   .hint = "How the Mask scales the spots: by its luma, its colors, or its first "
                           "channel.",
                   .entries = mask_type_entries,
                   .entry_count = ARRAY_SIZE(mask_type_entries)},
    MS_SHOW(SHOW_SIZE, "ShowSize", "Show Size"),
};

// One render: images and settings of the kernels.
typedef struct {
    MsImage *source, *mask, *small[2], *glints, *labels[2], *rows, *buffers[3], *output;
    int has_mask, invert_mask, mask_type, blur, glare_blur, rounds, count, pad, res;
    float blur_frac, glare_blur_frac, threshold, add[3], from_alpha, cap, norm, bc[3];
    float scale, hue, saturation, source_opacity, under, affect_alpha;
} Job;

// The spots (thresholded, labelled), their impulses and the style's drawing, transformed;
// their product transformed back; the result, on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    // Glare Res Half: the Source and the Mask at half size first.
    MsImage *source = j->res == 2 ? j->small[0] : j->source;
    MsImage *mask = j->res == 2 ? (j->mask == j->source ? j->small[0] : j->small[1]) : j->mask;
    int status = 0;
    if (j->res == 2) {
        status = MS_KERNEL(MS_K_GLARE_HALF, api, MS_IMAGE("image", j->source),
                           MS_IMAGE("half", j->small[0]));
        if (status == 0 && mask == j->small[1])
            status = MS_KERNEL(MS_K_GLARE_HALF, api, MS_IMAGE("image", j->mask),
                               MS_IMAGE("half", j->small[1]));
    }
    if (status == 0)
        status = MS_KERNEL(MS_K_GLINT_SOURCE, api, MS_IMAGE("source", source),
                           MS_IMAGE("mask", mask), MS_INT("has_mask", j->has_mask),
                           MS_INT("invert_mask", j->invert_mask), MS_INT("mask_type", j->mask_type),
                           MS_FLOAT("from_alpha", j->from_alpha), MS_INT("blur", j->blur),
                           MS_FLOAT("blur_frac", j->blur_frac), MS_FLOAT("threshold", j->threshold),
                           MS_FLOAT("add_r", j->add[0]), MS_FLOAT("add_g", j->add[1]),
                           MS_FLOAT("add_b", j->add[2]), MS_IMAGE("glints", j->glints));
    if (status == 0)
        status = MS_KERNEL(MS_K_GLARE_LABEL_INIT, api, MS_IMAGE("glints", j->glints),
                           MS_IMAGE("labels", j->labels[0]));
    for (int i = 0; i < j->rounds && status == 0; ++i)
        status = MS_KERNEL(MS_K_GLARE_LABEL_ROUND, api, MS_IMAGE("labels", j->labels[i % 2]),
                           MS_IMAGE("out", j->labels[(i + 1) % 2]));
    MsImage *drawing = j->buffers[0], *image = j->buffers[1], *free_image = j->buffers[2];
    if (status == 0)
        status = MS_KERNEL(MS_K_GLARE_KERNEL, api, MS_IMAGE("rows", j->rows),
                           MS_INT("count", j->count), MS_IMAGE("kernel", drawing));
    if (status == 0)
        status = ms_fft(api, &drawing, &image, 0);
    // `image` is free again (the scratch of the drawing's transform).
    if (status == 0)
        status = MS_KERNEL(MS_K_GLARE_SPOTS, api, MS_IMAGE("glints", j->glints),
                           MS_IMAGE("labels", j->labels[j->rounds % 2]), MS_FLOAT("cap", j->cap),
                           MS_FLOAT("norm", j->norm), MS_FLOAT("bc_r", j->bc[0]),
                           MS_FLOAT("bc_g", j->bc[1]), MS_FLOAT("bc_b", j->bc[2]),
                           MS_INT("pad_x", j->pad), MS_INT("pad_y", j->pad),
                           MS_IMAGE("impulses", image));
    if (status == 0)
        status = ms_fft(api, &image, &free_image, 0);
    if (status == 0)
        status = MS_KERNEL(MS_K_RACK_DEFOCUS_MULTIPLY, api, MS_IMAGE("image", image),
                           MS_IMAGE("lens", drawing), MS_IMAGE("product", free_image));
    if (status == 0)
        status = ms_fft(api, &free_image, &image, 1);
    if (status != 0)
        return status;
    return MS_KERNEL(MS_K_GLARE_FINISH, api, MS_IMAGE("data", free_image),
                     MS_IMAGE("source", j->source), MS_INT("pad_x", j->pad),
                     MS_INT("pad_y", j->pad), MS_INT("res", j->res), MS_FLOAT("scale", j->scale),
                     MS_FLOAT("hue", j->hue), MS_FLOAT("saturation", j->saturation),
                     MS_INT("blur", j->glare_blur), MS_FLOAT("blur_frac", j->glare_blur_frac),
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
    // Glare Res Half: the spots and the glare at half size (w2 x h2), the widths halved.
    const int res = (int)v[GLARE_RES] == 1 ? 2 : 1;
    const int w2 = (w + res - 1) / res, h2 = (h + res - 1) / res;
    const char *style = r->texts && r->texts[STYLE] ? r->texts[STYLE] : params[STYLE].def_text;
    Job j = {.has_mask = has_mask,
             .invert_mask = (int)v[INVERT_MASK],
             .mask_type = (int)v[MASK_TYPE],
             .threshold = (float)v[THRESHOLD],
             .from_alpha = (float)v[FROM_ALPHA],
             .hue = (float)(v[HUE_SHIFT] - floor(v[HUE_SHIFT])),
             .saturation = (float)v[SATURATION],
             .source_opacity = (float)v[SOURCE_OPACITY],
             .under = (float)v[UNDER_SOURCE],
             .affect_alpha = (float)v[AFFECT_ALPHA],
             .res = res};
    split(v[THRESHOLD_BLUR] / res, &j.blur, &j.blur_frac);
    split(v[BLUR_GLARE], &j.glare_blur, &j.glare_blur_frac);
    for (int c = 0; c < 3; ++c) {
        j.add[c] = (float)v[ADD_COLOR + c];
        j.bc[c] = (float)(v[BRIGHTNESS] * v[SCALE_COLORS + c]);
    }
    // A spot's strength: at most Size^2 / 100; strength (frame width / 10)^2 / 100 draws the
    // elements at their brightness.
    const double size = fmax(v[SIZE], 0.0) / res, frame = r->frame_width / res;
    j.cap = (float)(size * size / 100.0);
    j.norm = (float)fmax(frame * frame / 10000.0, 1e-6);
    // Enough rounds for the regions' labels to settle (each round follows the labels twice).
    j.rounds = 4;
    for (int n = w2 > h2 ? w2 : h2; n > 1; n /= 2)
        j.rounds += 2;

    // The style's rows; the drawing's reach gives the transforms' margin (nothing wraps around).
    const size_t text_length = strlen(style);
    MsJson *nodes = malloc((text_length + 1) * sizeof(MsJson));
    float (*rows)[GLARE_FIELDS] = calloc(GLARE_MAX_ELEMENTS, sizeof *rows);
    const MsGlareGlobals globals = {.size = size,
                                    .rel_height = v[REL_HEIGHT],
                                    .rotate = v[ROTATE],
                                    .rays_num_scale = v[RAYS_NUM_SCALE],
                                    .rays_length = v[RAYS_LENGTH],
                                    .rays_thickness = v[RAYS_THICKNESS]};
    const int allocated = nodes && rows;
    j.count = allocated ? ms_glare_style(style, nodes, (int)text_length + 1, &globals, rows,
                                         GLARE_MAX_ELEMENTS, NULL)
                        : -1;
    free(nodes);
    if (j.count < 0) {
        snprintf(r->err, r->err_size,
                 allocated ? "the Style is not a glare style (JSON)" : "not enough memory");
        free(rows);
        return 0;
    }
    double reach = 0;
    for (int i = 0; i < j.count; ++i) {
        const double s = fmax(rows[i][GLARE_SCALE_X], rows[i][GLARE_SCALE_Y]);
        reach = fmax(reach, rows[i][GLARE_R] * 1.6 * s + 4 * rows[i][GLARE_SIGMA] + 4);
    }
    // The image at (0, 0) of the transforms: the circular convolution does not wrap into it
    // while their size is at least the image's plus the drawing's reach (one side).
    const int longest = w2 > h2 ? w2 : h2;
    const int margin = (int)ceil(fmin(reach, (double)longest));
    j.pad = 0;
    const int nx = ms_fft_length(w2 + margin), ny = ms_fft_length(h2 + margin);
    j.scale = (float)(1.0 / ((double)nx * ny));

    // Between the kernels: the half-size Source and Mask, the glints, the regions' labels
    // (integers), the transforms.
    const size_t pixels = (size_t)w2 * h2, transform = (size_t)nx * ny * 4;
    const size_t floats[] = {res == 2 ? pixels * 4 : 0,
                             res == 2 && has_mask ? pixels * 4 : 0,
                             pixels * 4,
                             pixels,
                             pixels,
                             transform,
                             transform,
                             transform};
    float *block = malloc(ms_parts_size(floats, ARRAY_SIZE(floats))), *part[ARRAY_SIZE(floats)];
    if (!block) {
        free(rows);
        return ms_out_of_memory(r);
    }
    ms_parts(block, floats, ARRAY_SIZE(floats), part);
    MsImage source, mask, small_images[2], glints_image, label_images[2], rows_image, images[3],
        output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&mask, r->inputs[MASK].pixels, r->inputs[MASK].row, r->half, w, h);
    for (int i = 0; i < 2; ++i) {
        ms_image(&small_images[i], part[i], (ptrdiff_t)w2 * 4, w2, h2, 4);
        j.small[i] = &small_images[i];
    }
    ms_image(&glints_image, part[2], (ptrdiff_t)w2 * 4, w2, h2, 4);
    for (int i = 0; i < 2; ++i) {
        ms_image_uint(&label_images[i], part[3 + i], w2, w2, h2);
        j.labels[i] = &label_images[i];
    }
    ms_image(&rows_image, &rows[0][0], GLARE_FIELDS, GLARE_FIELDS, GLARE_MAX_ELEMENTS, 1);
    for (int i = 0; i < 3; ++i) {
        ms_image(&images[i], part[5 + i], (ptrdiff_t)nx * 4, nx, ny, 4);
        j.buffers[i] = &images[i];
    }
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    // Without a Mask, or one that is the Source's image in the host: the Source stands in.
    const int mask_is_source = has_mask && ms_same_input(&r->inputs[MASK], &r->inputs[SOURCE]);
    j.source = &source;
    j.mask = has_mask && !mask_is_source ? &mask : &source;
    j.glints = &glints_image;
    j.rows = &rows_image;
    j.output = &output;
    // Inputs uploaded to the GPU: the Source, the Mask, the style's rows.
    MsImage *const inputs[] = {&source, j.mask, &rows_image};
    MsImage *const between[] = {&glints_image,
                                &label_images[0],
                                &label_images[1],
                                &images[0],
                                &images[1],
                                &images[2],
                                part[0] ? &small_images[0] : NULL,
                                part[1] ? &small_images[1] : NULL};
    const int ok = ms_run(run, &j, inputs, ARRAY_SIZE(inputs), between, ARRAY_SIZE(between),
                          &output, r->info, r->err, r->err_size);
    free(block);
    free(rows);
    return ok;
}

// No glare (Size 0, Brightness 0, or Scale Colors 0) with the Source at full opacity: the
// Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    const int no_glare =
        v[SIZE] <= 0.0 || v[BRIGHTNESS] == 0.0 ||
        (v[SCALE_COLORS] == 0.0 && v[SCALE_COLORS_G] == 0.0 && v[SCALE_COLORS_B] == 0.0);
    return no_glare && v[SOURCE_OPACITY] == 1.0;
}

// The viewer: the Size and the Rel Height (the arrow).
static const MsOverlay overlay[] = {
    MS_OVERLAY_SIZED(MS_OVERLAY_ELLIPSE, "ShowSize", "size", NULL, NULL, "rel_height", 0.0,
                     MS_OVERLAY_ORANGE),
    MS_OVERLAY_SIZED(MS_OVERLAY_ARROW_Y, "ShowSize", "size", NULL, NULL, "rel_height", 0.0,
                     MS_OVERLAY_ORANGE),
};

const MsEffect ms_glare_effect = {
    .id = MS_ID("Glare"),
    .label = "MS_Glare",
    .description = "A glare (glows, rays, sectors, rings) on each bright spot of the image.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    .identity = identity,
    MS_OVERLAYS(overlay),
    .presets = "glare",
};
