// MS_EdgeAwareBlur: blurs regions of similar color while keeping the edges between
// them. It is a Domain Transform filter (Gastal & Oliveira, SIGGRAPH 2011), in Halide
// (src/halide/dt_generator.cpp), with its preparation (Edge Smooth, distances) on the GPU
// when the filter is (Sharp and Softer when possible, Soft where the profile says so):
//   - Blur Type: Sharp = normalised convolution, Soft = recursive, Softer = interpolated
//   - Filter: number of passes, Box 1, Triangle 2, Gauss 3, Smooth 5
//   - edge contrast = sum of the RGB differences (with Use Alpha Edges: 0.75 x the
//     sum of the RGBA differences) of the Edge Source (or the Source), blurred by
//     Edge Smooth; distance across an edge = 1 + contrast x blur / (3 x Edge Threshold),
//     with the blur width of that direction (Blur Rel included)
//   - Mask m: distances are 1/m + contrast term (m = smaller value of two neighbours),
//     so a darker mask blurs less and black does not blur
// Alpha is blurred like RGB. No blur, or an Edge Threshold of 0: the Source as it is (hosts
// use it without rendering).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/profile.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Edge_Source", "Edge_Source", 0.0f, 1, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
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
                     .max = MS_UNBOUNDED,
                     .display_max = 0.5},
    [REL_X] = {.type = MS_PARAM_DOUBLE,
               .name = "blur_rel_x",
               .label = "Blur Rel X",
               .hint = "Relative horizontal blur width.",
               .def = 1.0,
               .min = 0.0,
               .max = MS_UNBOUNDED,
               .display_max = 5.0},
    [REL_Y] = {.type = MS_PARAM_DOUBLE,
               .name = "blur_rel_y",
               .label = "Blur Rel Y",
               .hint = "Relative vertical blur width.",
               .def = 1.0,
               .min = 0.0,
               .max = MS_UNBOUNDED,
               .display_max = 5.0},
    [EDGE_THRESHOLD] =
        {.type = MS_PARAM_DOUBLE,
         .name = "edge_threshold",
         .label = "Edge Threshold",
         .hint =
             "Color regions separated by an edge larger than this will not blur into each other.",
         .def = 0.1,
         .min = 0.0,
         .max = MS_UNBOUNDED,
         .display_max = 1.0},
    [EDGE_SMOOTH] = {.type = MS_PARAM_WIDTH,
                     .name = "edge_smooth",
                     .label = "Edge Smooth",
                     .hint =
                         "Blur the Edge Source by this amount before calculating edge strength.",
                     .min = 0.0,
                     .max = MS_UNBOUNDED,
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

typedef enum {
    MS_DT_SHARP,  // normalised convolution: box in the transformed domain
    MS_DT_SOFT,   // recursive filter: exponential in the transformed domain
    MS_DT_SOFTER, // interpolated convolution
} MsDtType;

typedef struct {
    MsDtType type;
    int passes;            // Box 1, Triangle 2, Gauss 3, Smooth 5
    double blur_x, blur_y; // width in pixels per direction (0: no blur that way)
} MsDtSettings;

typedef struct {
    MsDtSettings dt;
    double scale_x, scale_y; // distance per unit of contrast, per direction
    int use_alpha;
    double edge_smooth; // pixels
    int has_edge, has_mask, mask_alpha, invert_mask;
} Settings;

// ---------------------------------------------------------------- kernels

// The filter: its preparation, its passes and what their kernels need
// (src/halide/dt_generator.cpp).
typedef struct {
    const Settings *s;
    const MsDtSettings *dt;
    MsImage *edge, *mask, *smooth;      // preparation: the edge image, maybe smoothed, the Mask
    int smooth_radius;                  // Edge Smooth in pixels, rounded (0: none)
    MsImage *source, *output, *scratch; // images
    MsImage *dx, *dy, *wx, *wy;         // distances, and Soft's weights
    MsImage *dy_turned, *turned;        // GPU: dy and an image, transposed (GPU memory only)
    double radius_x, radius_y;          // Sharp and Softer: (blur + N) / N
    int span_x, span_y, span_wide, reach_x, reach_y;
    int margin; // GPU: the samples a window can reach beyond its pixel, per side
} Passes;

// GPU Sharp and Softer (dt_generator.cpp: DtSharpGpu, DtSofterGpu): the block versions hold
// 256 + 2 x margin samples of 20 bytes in on-chip memory, at most 32 KB on Metal (margins up
// to BLOCK_MARGIN_LIMIT); the "rows" versions keep their running sums per row in GPU memory,
// at the same cost whatever the margin. The rows versions from the profile's rows_margin on
// (48 on an M1 Pro: they are faster from there); below it, Sharp's "short" block version
// (64 + 2 x margin samples) up to margins of sharp_short_margin (profile.h).
#define BLOCK_MARGIN_LIMIT 600

// The image pass `pass` writes: the output for the last pass, else alternately the scratch
// image and the output, so that a pass never reads the image it writes.
static MsImage *target(const Passes *p, int pass) {
    return (p->dt->passes - 1 - pass) % 2 == 0 ? p->output : p->scratch;
}

// One direction of a GPU pass: filters the rows of `in` (`columns`: the columns, as rows of
// the transposed image) and writes them transposed to `out`.
static int gpu_rows(MsApi api, const Passes *p, int columns, MsImage *in, MsImage *out) {
    MsImage *dist = columns ? p->dy_turned : p->dx;
    const double radius = columns ? p->radius_y : p->radius_x;
    const int span = columns ? p->span_y : p->span_x;
    const int reach = columns ? p->reach_y : p->reach_x;
    const int rows = p->margin >= ms_profile(api)->rows_margin || p->margin > BLOCK_MARGIN_LIMIT;
    if (p->dt->type == MS_DT_SOFTER)
        return MS_KERNEL(rows ? MS_K_DT_SOFTER_ROWS_GPU : MS_K_DT_SOFTER_GPU, api,
                         MS_IMAGE("input", in), MS_IMAGE("dist", dist), MS_FLOAT("half", radius),
                         MS_INT("span", reach), MS_IMAGE("output", out));
    const int short_tiles = p->margin <= ms_profile(api)->sharp_short_margin;
    return MS_KERNEL(rows          ? MS_K_DT_SHARP_ROWS_GPU
                     : short_tiles ? MS_K_DT_SHARP_SHORT_GPU
                                   : MS_K_DT_SHARP_GPU,
                     api, MS_IMAGE("input", in), MS_IMAGE("dist", dist), MS_FLOAT("radius", radius),
                     MS_INT("span", span), MS_INT("span_wide", p->span_wide),
                     MS_IMAGE("output", out));
}

// One pass on `api`. GPU Sharp and Softer: the rows, then the columns as rows of the
// transposed image; GPU Soft: rows then columns, in place. CPU: in double, exact (Soft: float).
static int pass(MsApi api, const Passes *p, MsImage *in, MsImage *out) {
    if (api != MS_API_CPU && p->dt->type != MS_DT_SOFT) {
        const int status = gpu_rows(api, p, 0, in, p->turned);
        return status != 0 ? status : gpu_rows(api, p, 1, p->turned, out);
    }
    switch (p->dt->type) {
    case MS_DT_SHARP:
        return MS_KERNEL(MS_K_DT_SHARP, api, MS_IMAGE("input", in), MS_IMAGE("dx", p->dx),
                         MS_IMAGE("dy", p->dy), MS_DOUBLE("radius_x", p->radius_x),
                         MS_DOUBLE("radius_y", p->radius_y), MS_INT("span_x", p->span_x),
                         MS_INT("span_y", p->span_y), MS_INT("span_wide", p->span_wide),
                         MS_IMAGE("output", out));
    case MS_DT_SOFT: // the same kernel on the GPU
        return MS_KERNEL(MS_K_DT_SOFT, api, MS_IMAGE("input", in), MS_IMAGE("wx", p->wx),
                         MS_IMAGE("wy", p->wy), MS_IMAGE("output", out));
    default:
        return MS_KERNEL(MS_K_DT_SOFTER, api, MS_IMAGE("input", in), MS_IMAGE("dx", p->dx),
                         MS_IMAGE("dy", p->dy), MS_DOUBLE("half_x", p->radius_x),
                         MS_DOUBLE("half_y", p->radius_y), MS_INT("span_x", p->reach_x),
                         MS_INT("span_y", p->reach_y), MS_IMAGE("output", out));
    }
}

// The preparation on `api`: Edge Smooth, the distances and (CPU) Soft's weights.
static int prepare(MsApi api, const Passes *p) {
    const Settings *s = p->s;
    const MsDtSettings *dt = p->dt;
    MsImage *edges = p->edge;
    int status = 0;
    if (p->smooth_radius > 0) {
        status = MS_KERNEL(MS_K_DT_EDGE_SMOOTH, api, MS_IMAGE("edge", p->edge),
                           MS_INT("radius", p->smooth_radius), MS_IMAGE("output", p->smooth));
        edges = p->smooth;
    }
    // Floor of the mask: 0.372 / (blur + passes), e.g. 0.0177 at 20 px Box; it only lets
    // 5e-4 through where m = 0.
    const double n = dt->passes;
    if (status == 0)
        status =
            MS_KERNEL(MS_K_DT_DISTANCES, api, MS_IMAGE("edge", edges), MS_IMAGE("mask", p->mask),
                      MS_INT("has_mask", s->has_mask), MS_INT("mask_alpha", s->mask_alpha),
                      MS_INT("invert_mask", s->invert_mask), MS_INT("use_alpha", s->use_alpha),
                      MS_FLOAT("scale_x", s->scale_x), MS_FLOAT("scale_y", s->scale_y),
                      MS_FLOAT("floor_x", 0.372 / (dt->blur_x + n)),
                      MS_FLOAT("floor_y", 0.372 / (dt->blur_y + n)), MS_IMAGE("dx", p->dx),
                      MS_IMAGE("dy", p->dy));
    if (status == 0 && dt->type == MS_DT_SOFT) {
        // a = exp(-sqrt(2) / sigma), sigma = (blur + N) / (2N) per pass (see
        // dt_generator.cpp); no blur in a direction: weight 0.
        const double log_a_x = dt->blur_x > 0 ? -sqrt(2.0) * 2 * n / (dt->blur_x + n) : -1e30;
        const double log_a_y = dt->blur_y > 0 ? -sqrt(2.0) * 2 * n / (dt->blur_y + n) : -1e30;
        status = MS_KERNEL(MS_K_DT_SOFT_WEIGHTS, api, MS_IMAGE("dx", p->dx), MS_IMAGE("dy", p->dy),
                           MS_FLOAT("log_a_x", log_a_x), MS_FLOAT("log_a_y", log_a_y),
                           MS_IMAGE("wx", p->wx), MS_IMAGE("wy", p->wy));
    }
    return status;
}

// The preparation, then all the passes, on `api`; on the GPU the images stay there in
// between. The GPU takes Sharp, and Soft and Softer where the profile says so.
static int run_passes(MsApi api, void *user) {
    const Passes *p = user;
    const MsDtType type = p->dt->type;
    if (api != MS_API_CPU) {
        const MsProfile *profile = ms_profile(api);
        if ((type == MS_DT_SOFT && !profile->soft_gpu) ||
            (type == MS_DT_SOFTER && !profile->softer_gpu))
            return MS_RUN_DECLINE;
    }
    int status = prepare(api, p);
    // GPU Sharp and Softer: the columns' distances, transposed like the image between the two
    // directions.
    if (status == 0 && api != MS_API_CPU && type != MS_DT_SOFT)
        status = MS_KERNEL(MS_K_DT_TRANSPOSE, api, MS_IMAGE("input", p->dy),
                           MS_IMAGE("output", p->dy_turned));
    MsImage *from = p->source;
    for (int i = 0; status == 0 && i < p->dt->passes; ++i) {
        MsImage *to = target(p, i);
        status = pass(api, p, from, to);
        from = to;
    }
    return status;
}

// The largest mask value on the CPU, then the preparation and the filter (run_passes) on the
// first API that takes them. Returns 1 on success, or 0 with the reason in err.
static int process(const MsRender *r, const Settings *s) {
    const int width = r->width, height = r->height;
    const size_t pixels = (size_t)width * height;
    const MsDtSettings *dt = &s->dt;
    const int soft = dt->type == MS_DT_SOFT;
    const int smooth_radius = (int)floor(s->edge_smooth + 0.5); // as the blur rounds it
    // The images between the kernels, in one allocation (each part on 64 bytes): the
    // distances dx, dy; Edge Smooth's image; a scratch image between passes; Soft's weights
    // a^d, the same for every pass.
    const size_t floats[6] = {pixels,
                              pixels,
                              smooth_radius > 0 ? pixels * 4 : 0,
                              dt->passes > 1 ? pixels * 4 : 0,
                              soft ? pixels : 0,
                              soft ? pixels : 0};
    size_t total = 0;
    for (int i = 0; i < 6; ++i)
        total += (floats[i] + 15) / 16 * 16;
    float *block = malloc(total * sizeof(float));
    if (!block) {
        snprintf(r->err, r->err_size, "not enough memory");
        return 0;
    }
    float *part[6];
    size_t at = 0;
    for (int i = 0; i < 6; ++i) {
        part[i] = floats[i] ? block + at : NULL;
        at += (floats[i] + 15) / 16 * 16;
    }
    float *dx = part[0], *dy = part[1], *smooth = part[2], *scratch = part[3], *wx = part[4],
          *wy = part[5];
    const MsInput *source_in = &r->inputs[SOURCE];
    MsImage source, edge, mask, output, ismooth, iscratch, idx, idy, iwx, iwy, ilargest;
    MsImage dy_turned, turned;
    ms_image_rgba(&source, source_in->pixels, source_in->row, r->half, width, height);
    ms_image_rgba(&edge, r->inputs[EDGE].pixels, r->inputs[EDGE].row, r->half, width, height);
    ms_image_rgba(&mask, r->inputs[MASK].pixels, r->inputs[MASK].row, r->half, width, height);
    ms_image_rgba(&output, r->output, r->output_row, r->half, width, height);
    ms_image(&ismooth, smooth, (ptrdiff_t)width * 4, width, height, 4);
    ms_image(&iscratch, scratch, (ptrdiff_t)width * 4, width, height, 4);
    ms_image(&idx, dx, width, width, height, 1);
    ms_image(&idy, dy, width, width, height, 1);
    ms_image(&iwx, wx, width, width, height, 1);
    ms_image(&iwy, wy, width, width, height, 1);
    float largest = 1.0f;
    ms_image(&ilargest, &largest, 1, 1, 1, 1);
    ilargest.b.dimensions = 1;
    ms_image(&dy_turned, NULL, height, height, width, 1);
    ms_image(&turned, NULL, (ptrdiff_t)height * 4, height, width, 4);
    // Without an Edge Source, the Source is the edge image; without a Mask, the distances do
    // not read it: the edge image stands in. An input that is the same image as an earlier
    // one in the host: that one stands in (uploaded once).
    const MsInput *in = r->inputs;
    MsImage *edges = s->has_edge && !ms_same_input(&in[EDGE], &in[SOURCE]) ? &edge : &source;
    MsImage *masks = !s->has_mask                              ? edges
                     : ms_same_input(&in[MASK], &in[SOURCE]) ? &source
                     : ms_same_input(&in[MASK], &in[EDGE])   ? edges
                                                             : &mask;

    int status = 0;
    // The largest mask value, on the CPU: a Mask in 16-bit floats converted first.
    float *mask_floats = s->has_mask && r->half ? malloc(pixels * 4 * sizeof(float)) : NULL;
    MsImage mask_half, mask_float;
    if (s->has_mask && r->half && !mask_floats)
        status = halide_error_code_out_of_memory;
    if (mask_floats) {
        ms_image_half(&mask_half, r->inputs[MASK].pixels, r->inputs[MASK].row, width, height);
        ms_image(&mask_float, mask_floats, (ptrdiff_t)width * 4, width, height, 4);
        status = MS_KERNEL(MS_K_TO_FLOAT, MS_API_CPU, MS_IMAGE("input", &mask_half),
                           MS_IMAGE("output", &mask_float));
    }
    if (s->has_mask && status == 0)
        status = MS_KERNEL(MS_K_DT_LARGEST, MS_API_CPU,
                           MS_IMAGE("mask", mask_floats ? &mask_float : &mask),
                           MS_INT("mask_alpha", s->mask_alpha),
                           MS_INT("invert_mask", s->invert_mask), MS_IMAGE("largest", &ilargest));
    free(mask_floats);
    int ok = status == 0;
    if (!ok)
        snprintf(r->err, r->err_size, "Halide error %d", status);

    if (ok) {
        // Sharp and Softer: width (blur + N) / N per pass; a window holds at most
        // width / min_distance samples per side, min_distance = 1 / (largest mask value),
        // and the segment holding ct +- h (Softer) can be one sample further.
        const double n = dt->passes;
        const double min_distance = 1.0 / largest;
        Passes p = {.s = s,
                    .dt = dt,
                    .edge = edges,
                    .mask = masks,
                    .smooth = &ismooth,
                    .smooth_radius = smooth_radius,
                    .source = &source,
                    .output = &output,
                    .scratch = &iscratch,
                    .dx = &idx,
                    .dy = &idy,
                    .wx = &iwx,
                    .wy = &iwy,
                    .dy_turned = &dy_turned,
                    .turned = &turned};
        p.radius_x = dt->blur_x > 0 ? (dt->blur_x + n) / n : 0.0;
        p.radius_y = dt->blur_y > 0 ? (dt->blur_y + n) / n : 0.0;
        p.span_x = (int)floor(p.radius_x / min_distance + 1e-6) + 1;
        p.span_y = (int)floor(p.radius_y / min_distance + 1e-6) + 1;
        p.span_wide = (int)ceil(1.0 / min_distance) + 1; // one more unit of distance
        p.reach_x = (int)ceil(p.radius_x / min_distance) + 1;
        p.reach_y = (int)ceil(p.radius_y / min_distance) + 1;
        const int span = p.span_x > p.span_y ? p.span_x : p.span_y;
        const int reach = p.reach_x > p.reach_y ? p.reach_x : p.reach_y;
        p.margin = dt->type == MS_DT_SHARP ? span + p.span_wide + 2 : reach + 3;
        // Uploaded: the Source, the Edge Source and the Mask when connected (ms_run uploads an
        // image standing in for another once).
        MsImage *const inputs[] = {&source, edges, masks};
        // Between the kernels: those of the preparation, of the passes, and of the GPU's
        // directions (Sharp, Softer) or Soft's weights.
        MsImage *between[8] = {&idx, &idy, &iscratch};
        int between_count = 3;
        if (smooth_radius > 0)
            between[between_count++] = &ismooth;
        between[between_count++] = soft ? &iwx : &dy_turned;
        between[between_count++] = soft ? &iwy : &turned;
        ok = ms_run(run_passes, &p, inputs, ARRAY_SIZE(inputs), between, between_count, &output,
                    r->info, r->err, r->err_size);
    }
    free(block);
    return ok;
}

// ---------------------------------------------------------------- render

// The blur type of the Blur Type menu.
static MsDtType blur_type(const double *v) {
    const int type = (int)v[BLUR_TYPE];
    return type == 1 ? MS_DT_SOFT : (type == 2 ? MS_DT_SOFTER : MS_DT_SHARP);
}

// The blur width in pixels per direction (Sharp without Subpixel: whole pixels).
static void blur_widths(const double *v, double *x, double *y) {
    *x = v[BLUR_AMOUNT] * v[REL_X];
    *y = v[BLUR_AMOUNT] * v[REL_Y];
    if (blur_type(v) == MS_DT_SHARP && !v[SUBPIXEL]) {
        *x = floor(*x + 0.5);
        *y = floor(*y + 0.5);
    }
}

static int render(const MsRender *r) {
    const double *v = r->values;
    Settings s;
    const double threshold = v[EDGE_THRESHOLD];
    const int filter = (int)v[FILTER];
    s.dt.type = blur_type(v);
    s.dt.passes = filter_passes[filter >= 0 && filter < 4 ? filter : 2];
    blur_widths(v, &s.dt.blur_x, &s.dt.blur_y);
    s.scale_x = threshold > 0 ? s.dt.blur_x / (3.0 * threshold) : 1e12;
    s.scale_y = threshold > 0 ? s.dt.blur_y / (3.0 * threshold) : 1e12;
    s.use_alpha = (int)v[USE_ALPHA];
    s.edge_smooth = v[EDGE_SMOOTH];
    s.has_edge = r->connected[EDGE];
    s.has_mask = r->connected[MASK];
    s.mask_alpha = (int)v[MASK_USE];
    s.invert_mask = (int)v[INVERT_MASK];
    return process(r, &s);
}

// No blur either way, or an Edge Threshold of 0 (every pixel an edge, except between equal
// pixels, which their blur leaves as they are): the Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    double blur_x, blur_y;
    blur_widths(v, &blur_x, &blur_y);
    return (blur_x <= 0 && blur_y <= 0) || v[EDGE_THRESHOLD] <= 0;
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
    identity,
};
