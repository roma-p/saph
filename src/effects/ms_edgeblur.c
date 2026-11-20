// MS_EdgeBlur: blurs the image along its edges (src/halide/edge_blur_generator.cpp).
//   - the edges: the luma of the Matte (the Source without one; Matte Use Alpha: the Matte's
//     alpha), blurred by Edge Width, the length of its gradient times
//     Edge Strength x (0.8 x Edge Width + frame width / 100), less Edge Threshold, in [0, 1]
//   - the Source blurred by Blur Width (3 box blurs, mirrored borders), mixed over it by the
//     edges; Show Edges outputs the edges (alpha 1)
// Blur Width and Edge Width (Subpixel: between whole radii) are "normalised" values, so
// hosts show them in pixels.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/defines.h"
#include "common/effect.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Matte", "Matte", 0.0f, 1, 0},
};
enum { SOURCE, MATTE };

static const char *const show_entries[] = {"Result", "Edges"};
static const char *const matte_entries[] = {"Luma", "Alpha"};

enum { BLUR_WIDTH, EDGE_WIDTH, EDGE_STRENGTH, EDGE_THRESHOLD, SHOW, SUBPIXEL, MATTE_USE };
static const MsParam params[] = {
    [BLUR_WIDTH] = {.type = MS_PARAM_WIDTH,
                    .name = "blur_width",
                    .label = "Blur Width",
                    .hint = "How much the edges are blurred.",
                    .def = 0.014,
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 0.1},
    [EDGE_WIDTH] = {.type = MS_PARAM_WIDTH,
                    .name = "edge_width",
                    .label = "Edge Width",
                    .hint = "The width of the edges found and blurred.",
                    .def = 0.014,
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 0.1},
    [EDGE_STRENGTH] = {.type = MS_PARAM_DOUBLE,
                       .name = "edge_strength",
                       .label = "Edge Strength",
                       .hint = "Scales the edges: more of the image is blurred.",
                       .def = 0.5,
                       .min = 0.0,
                       .max = MS_UNBOUNDED,
                       .display_max = 5.0},
    [EDGE_THRESHOLD] = {.type = MS_PARAM_DOUBLE,
                        .name = "edge_threshold",
                        .label = "Edge Threshold",
                        .hint = "Edges weaker than this are left out.",
                        .min = 0.0,
                        .max = MS_UNBOUNDED,
                        .display_max = 1.0},
    [SHOW] = {.type = MS_PARAM_CHOICE,
              .name = "show",
              .label = "Show",
              .hint = "The result, or the edges where the image is blurred.",
              .entries = show_entries,
              .entry_count = ARRAY_SIZE(show_entries)},
    [SUBPIXEL] = {.type = MS_PARAM_BOOL,
                  .name = "subpixel",
                  .label = "Subpixel",
                  .hint = "Widths between whole pixels, for smooth animation.",
                  .def = 1.0},
    [MATTE_USE] = {.type = MS_PARAM_CHOICE,
                   .name = "matte_use",
                   .label = "Matte Use",
                   .hint = "The edges of the Matte's luma or of its alpha.",
                   .entries = matte_entries,
                   .entry_count = ARRAY_SIZE(matte_entries)},
};

// One render: images and settings of the kernels (src/halide/edge_blur_generator.cpp).
typedef struct {
    MsImage *source, *matte, *edges, *output;
    int use_alpha, show_edges;
    int edge_radius, blur_radius; // total radii: whole pixels
    float edge_frac, blur_frac;   // and the fraction past them
    float scale, threshold;
} Job;

// The edges, then the result, on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    const int status =
        MS_KERNEL(MS_K_EDGE_BLUR_EDGES, api, MS_IMAGE("matte", j->matte),
                  MS_INT("use_alpha", j->use_alpha), MS_INT("radius", j->edge_radius),
                  MS_FLOAT("frac", j->edge_frac), MS_FLOAT("scale", j->scale),
                  MS_FLOAT("threshold", j->threshold), MS_IMAGE("edges", j->edges));
    if (status != 0)
        return status;
    return MS_KERNEL(MS_K_EDGE_BLUR, api, MS_IMAGE("source", j->source),
                     MS_IMAGE("edges", j->edges), MS_INT("radius", j->blur_radius),
                     MS_FLOAT("frac", j->blur_frac), MS_INT("show_edges", j->show_edges),
                     MS_IMAGE("output", j->output));
}

// A total radius `t`: whole pixels (Subpixel: the fraction past them in *frac, else rounded).
static int radius(double t, int subpixel, float *frac) {
    t = t > 0 ? (t < 1e7 ? t : 1e7) : 0;
    const double whole = subpixel ? floor(t) : floor(t + 0.5);
    *frac = (float)(subpixel ? t - whole : 0.0);
    return (int)whole;
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height, has_matte = r->connected[MATTE];
    const int subpixel = v[SUBPIXEL] != 0, show_edges = (int)v[SHOW] == 1;
    float *edges = malloc((size_t)w * h * sizeof(float));
    if (!edges) {
        snprintf(r->err, r->err_size, "not enough memory");
        return 0;
    }
    MsImage source, matte, edge_image, output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    ms_image_rgba(&matte, r->inputs[MATTE].pixels, r->inputs[MATTE].row, r->half, w, h);
    ms_image(&edge_image, edges, w, w, h, 1);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    // Without a Matte, or one that is the Source's image in the host: the Source stands in
    // (uploaded once).
    const int matte_is_source = has_matte && ms_same_input(&r->inputs[MATTE], &r->inputs[SOURCE]);
    const double edge_width = v[EDGE_WIDTH] > 0 ? v[EDGE_WIDTH] : 0;
    Job j = {.source = &source,
             .matte = has_matte && !matte_is_source ? &matte : &source,
             .edges = &edge_image,
             .output = &output,
             .use_alpha = has_matte && (int)v[MATTE_USE] == 1,
             .show_edges = show_edges,
             .scale = (float)(v[EDGE_STRENGTH] * (0.8 * edge_width + r->frame_width / 100)),
             .threshold = (float)v[EDGE_THRESHOLD]};
    j.edge_radius = radius(edge_width, subpixel, &j.edge_frac);
    // Show Edges: no blur.
    j.blur_radius = show_edges ? 0 : radius(v[BLUR_WIDTH], subpixel, &j.blur_frac);
    // Inputs uploaded to the GPU: the Source, and the Matte.
    MsImage *const inputs[] = {&source, j.matte};
    MsImage *const between[] = {&edge_image};
    const int ok = ms_run(run, &j, inputs, 2, between, 1, &output, r->info, r->err, r->err_size);
    free(edges);
    return ok;
}

// No blur (Blur Width under a pixel, or Edge Strength 0) shown: the Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    float frac;
    const int no_blur = radius(v[BLUR_WIDTH], v[SUBPIXEL] != 0, &frac) == 0 && frac == 0.0f;
    return (int)v[SHOW] == 0 && (no_blur || v[EDGE_STRENGTH] == 0.0);
}

const MsEffect ms_edgeblur_effect = {
    MS_ID("EdgeBlur"),
    "MS_EdgeBlur",
    "Blurs the image along its edges.",
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
