// MS_EdgeAwareBlur: blurs regions of similar color while keeping the edges between
// them. It is a Domain Transform filter (see domain_transform.c), run on the CPU:
//   - Blur Type: Sharp = normalised convolution, Soft = recursive, Softer = interpolated
//   - Filter: number of passes, Box 1, Triangle 2, Gauss 3, Smooth 5
//   - edge contrast = sum of the RGB differences (with Use Alpha Edges: 0.75 x the
//     sum of the RGBA differences) of the Edge Source (or the Source), blurred by
//     Edge Smooth; distance across an edge = 1 + contrast x blur / (3 x Edge Threshold),
//     with the blur width of that direction (Blur Rel included)
//   - Mask m: distances are 1/m + contrast term (m = smaller value of two neighbours),
//     so a darker mask blurs less and black does not blur
// Alpha is blurred like RGB.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/blur.h"
#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/domain_transform.h"

// Inputs: clip, label in the host, name, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", "source", 0.0f, 0, 0},
    {"Edge_Source", "Edge_Source", "edge", 0.0f, 1, 0},
    {"Mask", "Mask", "mask", 0.0f, 1, 1},
};
enum { SOURCE, EDGE, MASK };

static const char *const types[] = {"Sharp Edges", "Soft Edges", "Softer Edges"};
static const char *const filters[] = {"Box", "Triangle", "Gauss", "Smooth"};
enum {
    BLUR_AMOUNT,
    REL_X,
    REL_Y,
    EDGE_THRESHOLD,
    EDGE_SMOOTH,
    USE_ALPHA,
    BLUR_TYPE,
    FILTER,
    SUBPIXEL,
    INVERT_MASK,
    MASK_USE,
};
static const MsParam params[] = {
    [BLUR_AMOUNT] = {.type = MS_PARAM_WIDTH,
                     .name = "blur_amount",
                     .label = "Blur Amount",
                     .hint = "Scales the width of the blur.",
                     .def = 0.05,
                     .min = 0.0,
                     .max = INFINITY,
                     .display_max = 0.5},
    [REL_X] = {.type = MS_PARAM_DOUBLE,
               .name = "blur_rel_x",
               .label = "Blur Rel X",
               .hint = "Relative horizontal blur width.",
               .def = 1.0,
               .min = 0.0,
               .max = INFINITY,
               .display_max = 5.0},
    [REL_Y] = {.type = MS_PARAM_DOUBLE,
               .name = "blur_rel_y",
               .label = "Blur Rel Y",
               .hint = "Relative vertical blur width.",
               .def = 1.0,
               .min = 0.0,
               .max = INFINITY,
               .display_max = 5.0},
    [EDGE_THRESHOLD] =
        {.type = MS_PARAM_DOUBLE,
         .name = "edge_threshold",
         .label = "Edge Threshold",
         .hint =
             "Color regions separated by an edge larger than this will not blur into each other.",
         .def = 0.1,
         .min = 0.0,
         .max = INFINITY,
         .display_max = 1.0},
    [EDGE_SMOOTH] = {.type = MS_PARAM_WIDTH,
                     .name = "edge_smooth",
                     .label = "Edge Smooth",
                     .hint =
                         "Blur the Edge Source by this amount before calculating edge strength.",
                     .min = 0.0,
                     .max = INFINITY,
                     .display_max = 0.0125},
    [USE_ALPHA] = {.type = MS_PARAM_BOOL,
                   .name = "use_alpha_edges",
                   .label = "Use Alpha Edges",
                   .hint = "Include the alpha channel when calculating edge strength."},
    [BLUR_TYPE] = {.type = MS_PARAM_CHOICE,
                   .name = "blur_type",
                   .label = "Blur Type",
                   .hint = "Determines the style of blur effect.",
                   .entries = types,
                   .entry_count = 3},
    [FILTER] = {.type = MS_PARAM_CHOICE,
                .name = "filter",
                .label = "Filter",
                .hint = "The type of convolution filter to blur with.",
                .def = 2,
                .entries = filters,
                .entry_count = 4},
    [SUBPIXEL] = {.type = MS_PARAM_BOOL,
                  .name = "subpixel",
                  .label = "Subpixel",
                  .hint = "Enables blurring by subpixel amounts (Sharp Edges only).",
                  .def = 1},
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Apply the effect where the Mask is black."),
    [MASK_USE] = MS_PARAM_MASK_USE,
};

static const int filter_passes[] = {1, 2, 3, 5}; // Box, Triangle, Gauss, Smooth

typedef struct {
    MsDtSettings dt;
    double scale_x, scale_y; // distance per unit of contrast, per direction
    int use_alpha;
    double edge_smooth; // pixels
    int has_edge, has_mask, mask_alpha, invert_mask;
} Settings;

// ---------------------------------------------------------------- processing

static int process(const MsInput *inputs, int width, int height, float *output, char *err,
                   size_t err_size, const Settings *s) {
    const size_t pixels = (size_t)width * height;
    const float *source = inputs[SOURCE].pixels;

    float *edge_copy = NULL; // Edge Smooth works on a copy of the edge image
    float *mask = NULL;
    float *dx = malloc(pixels * sizeof(float));
    float *dy = malloc(pixels * sizeof(float));
    int ok = dx && dy;
    const float *edge = s->has_edge ? inputs[EDGE].pixels : source;
    if (ok && s->edge_smooth > 0) {
        edge_copy = malloc(pixels * 4 * sizeof(float));
        ok = edge_copy != NULL;
        if (ok) {
            memcpy(edge_copy, edge, pixels * 4 * sizeof(float));
            ms_box3_blur(edge_copy, width, height, 4, s->edge_smooth);
            edge = edge_copy;
        }
    }
    if (ok && s->has_mask) {
        mask = malloc(pixels * sizeof(float));
        ok = mask != NULL;
        if (ok)
            ms_mask_values(inputs[MASK].pixels, pixels, s->mask_alpha, s->invert_mask, mask);
    }
    if (ok) {
        // Floor of the mask: 0.372 / (blur + passes), e.g. 0.0177 at 20 px Box; it only
        // lets 5e-4 through where m = 0.
        const double n = s->dt.passes;
        ms_dt_distances(edge, width, height, s->scale_x, s->scale_y, s->use_alpha, mask,
                        0.372 / (s->dt.blur_x + n), 0.372 / (s->dt.blur_y + n), dx, dy);
        memcpy(output, source, pixels * 4 * sizeof(float));
        ms_dt_filter(output, width, height, dx, dy, &s->dt);
    } else {
        snprintf(err, err_size, "not enough memory");
    }
    free(edge_copy);
    free(mask);
    free(dx);
    free(dy);
    return ok;
}

// ---------------------------------------------------------------- render

static int render(const MsRender *r) {
    const double *v = r->values;
    Settings s;
    const double blur = v[BLUR_AMOUNT], threshold = v[EDGE_THRESHOLD];
    const int type = (int)v[BLUR_TYPE], filter = (int)v[FILTER];
    s.dt.type = type == 1 ? MS_DT_SOFT : (type == 2 ? MS_DT_SOFTER : MS_DT_SHARP);
    s.dt.passes = filter_passes[filter >= 0 && filter < 4 ? filter : 2];
    s.dt.blur_x = blur * v[REL_X];
    s.dt.blur_y = blur * v[REL_Y];
    if (s.dt.type == MS_DT_SHARP && !v[SUBPIXEL]) { // whole pixels
        s.dt.blur_x = floor(s.dt.blur_x + 0.5);
        s.dt.blur_y = floor(s.dt.blur_y + 0.5);
    }
    s.scale_x = threshold > 0 ? s.dt.blur_x / (3.0 * threshold) : 1e12;
    s.scale_y = threshold > 0 ? s.dt.blur_y / (3.0 * threshold) : 1e12;
    s.use_alpha = (int)v[USE_ALPHA];
    s.edge_smooth = v[EDGE_SMOOTH];
    s.has_edge = r->connected[EDGE];
    s.has_mask = r->connected[MASK];
    s.mask_alpha = (int)v[MASK_USE];
    s.invert_mask = (int)v[INVERT_MASK];
    return process(r->inputs, r->width, r->height, r->output, r->err, r->err_size, &s);
}

const MsEffect ms_edgeawareblur_effect = {
    MS_ID("EdgeAwareBlur"),
    "MS_EdgeAwareBlur",
    "Blurs regions of similar color while preserving edges between regions of different colors.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
};
