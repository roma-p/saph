// MS_FlickerMatch and MS_FlickerMatchColor: the Source takes the flicker of the Match input.
// Each frame, the Source is scaled by the Match input's level in a rectangle over a target:
//   - FlickerMatch: the luma (5 R + 8 G + 3 B) / 16 of the Match input's mean over the
//     rectangle, over Match Level, scales the three colour channels
//   - FlickerMatchColor: each channel's mean over the rectangle, over that channel of Match
//     Color, scales that channel
//   - the rectangle between Rect Corner1 and Rect Corner2 (either way round): the pixels from
//     the lower corner's, truncated, to the one before the higher corner's (at least one),
//     kept inside the image
//   - alpha unchanged; without a Match input, the Source as it is
// (src/halide/flicker_match_generator.cpp)

#include <math.h>

#include "common/defines.h"
#include "common/effect.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Match", "Match", 0.0f, 1, 0},
};
enum { SOURCE, MATCH };

enum { CORNER1, CORNER1_Y, CORNER2, CORNER2_Y, TARGET, TARGET_G, TARGET_B };
// Show Rect: after Match Level, after Match Color's blue
enum { LEVEL_SHOW_RECT = TARGET + 1, COLOR_SHOW_RECT = TARGET_B + 1 };
#define SHOW_RECT(id) MS_SHOW(id, "ShowRect", "Show Rect")

#define CORNERS                                                                                    \
    [CORNER1] = {.type = MS_PARAM_POINT,                                                           \
                 .name = "rect_corner1",                                                           \
                 .label = "Rect Corner1",                                                          \
                 .hint = "One corner of the rectangle where the Match input's level is taken.",    \
                 .def = 0.2083},                                                                   \
    [CORNER1_Y] = {.type = MS_PARAM_POINT_Y, .name = "rect_corner1_y", .def = 0.2057},             \
    [CORNER2] = {.type = MS_PARAM_POINT,                                                           \
                 .name = "rect_corner2",                                                           \
                 .label = "Rect Corner2",                                                          \
                 .hint = "The opposite corner of the rectangle.",                                  \
                 .def = 0.7917},                                                                   \
    [CORNER2_Y] = {.type = MS_PARAM_POINT_Y, .name = "rect_corner2_y", .def = 0.7943}

static const MsParam level_params[] = {
    CORNERS,
    [TARGET] = {.type = MS_PARAM_DOUBLE,
                .name = "match_level",
                .label = "Match Level",
                .hint = "The Match input's level (luma in the rectangle) that leaves the Source "
                        "as it is: the Source is scaled by the level over this.",
                .def = 0.5,
                .min = 0.01,
                .max = MS_UNBOUNDED,
                .display_min = 0.01,
                .display_max = 2.0},
    SHOW_RECT(LEVEL_SHOW_RECT),
};

static const MsParam color_params[] = {
    CORNERS,
    [TARGET] = {.type = MS_PARAM_RGB,
                .name = "match_color",
                .label = "Match Color",
                .hint = "The Match input's colour (mean in the rectangle) that leaves the Source "
                        "as it is: each channel is scaled by the mean over this.",
                .def = 0.5},
    [TARGET_G] = {.type = MS_PARAM_RGB_PART, .name = "match_color_green", .def = 0.5},
    [TARGET_B] = {.type = MS_PARAM_RGB_PART, .name = "match_color_blue", .def = 0.5},
    SHOW_RECT(COLOR_SHOW_RECT),
};

// One render: images and settings of the kernel.
typedef struct {
    MsImage *source, *match, *output;
    int x0, y0, columns, rows, luma;
    float target[3];
} Job;

static int run(MsApi api, void *user) {
    const Job *j = user;
    return MS_KERNEL(MS_K_FLICKER_MATCH, api, MS_IMAGE("source", j->source),
                     MS_IMAGE("match", j->match), MS_INT("x0", j->x0), MS_INT("y0", j->y0),
                     MS_INT("columns", j->columns), MS_INT("rows", j->rows),
                     MS_INT("luma", j->luma), MS_FLOAT("target_r", j->target[0]),
                     MS_FLOAT("target_g", j->target[1]), MS_FLOAT("target_b", j->target[2]),
                     MS_IMAGE("output", j->output));
}

// The pixels [*first, *last] of `n` between the corners `a` and `b`.
static void span(int n, double a, double b, int *first, int *last) {
    const double lo = floor(fmin(a, b)), hi = fmax(floor(fmax(a, b)) - 1.0, lo);
    *first = (int)fmin(fmax(lo, 0.0), n - 1.0);
    *last = (int)fmin(fmax(hi, 0.0), n - 1.0);
}

static int render_with(const MsRender *r, int luma) {
    const double *v = r->values;
    const int w = r->width, h = r->height, has_match = r->connected[MATCH];
    MsImage source, match, output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&match, r->inputs[MATCH].pixels, r->inputs[MATCH].row, r->half, w, h);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    int x1, y1;
    // Without a Match input: no target, gains of 1 (the Source stands in for the Match).
    const int match_is_source = !has_match || ms_same_input(&r->inputs[MATCH], &r->inputs[SOURCE]);
    Job j = {.source = &source,
             .match = match_is_source ? &source : &match,
             .output = &output,
             .luma = luma,
             .target = {has_match ? (float)v[TARGET] : 0.0f,
                        has_match ? (float)v[luma ? TARGET : TARGET_G] : 0.0f,
                        has_match ? (float)v[luma ? TARGET : TARGET_B] : 0.0f}};
    span(w, v[CORNER1], v[CORNER2], &j.x0, &x1);
    span(h, v[CORNER1_Y], v[CORNER2_Y], &j.y0, &y1);
    j.columns = x1 - j.x0 + 1;
    j.rows = y1 - j.y0 + 1;
    // Inputs uploaded to the GPU: the Source, the Match input.
    MsImage *const inputs[] = {&source, j.match};
    return ms_run(run, &j, inputs, 2, NULL, 0, &output, r->info, r->err, r->err_size);
}

static int render_level(const MsRender *r) { return render_with(r, 1); }

static int render_color(const MsRender *r) { return render_with(r, 0); }

// Without a Match input: the Source as it is.
static int identity(const double *values, const int *connected) {
    (void)values;
    return !connected[MATCH];
}

// The viewer: the rectangle (its corners and sides drag them), and its middle (drags it
// whole).
static const MsOverlay overlay[] = {
    {.kind = MS_OVERLAY_RECT,
     .colour = MS_OVERLAY_ORANGE,
     .show = "ShowRect",
     .point = "rect_corner1",
     .point2 = "rect_corner2"},
    {.kind = MS_OVERLAY_CROSSHAIR,
     .colour = MS_OVERLAY_ORANGE,
     .show = "ShowRect",
     .point = "rect_corner1",
     .point2 = "rect_corner2",
     .factor = 10.0},
};

const MsEffect ms_flickermatch_effect = {
    MS_ID("FlickerMatch"),
    "MS_FlickerMatch",
    "Gives the Source the flicker of the Match input: scales it by the Match input's level in "
    "a rectangle over Match Level, frame by frame.",
    clips,
    ARRAY_SIZE(clips),
    level_params,
    ARRAY_SIZE(level_params),
    render_level,
    identity,
    0,
    NULL,
    overlay,
    ARRAY_SIZE(overlay),
    "flickermatch", // presets (common/preset.h)
    NULL,           // not varying with its settings
};

const MsEffect ms_flickermatchcolor_effect = {
    MS_ID("FlickerMatchColor"),
    "MS_FlickerMatchColor",
    "Gives the Source the colour flicker of the Match input: scales each channel by the Match "
    "input's mean in a rectangle over Match Color, frame by frame.",
    clips,
    ARRAY_SIZE(clips),
    color_params,
    ARRAY_SIZE(color_params),
    render_color,
    identity,
    0,
    NULL,
    overlay,
    ARRAY_SIZE(overlay),
    "flickermatchcolor", // presets (common/preset.h)
    NULL,                // not varying with its settings
};
