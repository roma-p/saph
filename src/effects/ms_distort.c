// MS_Distort: warps the Source by the slopes of a Lens image. Runs on the GPU (shaders
// distort_*.glsl), or on the CPU with Enable GPU off (same result, used as the
// reference):
//   - lens = luma (5/16, 8/16, 3/16) of the Lens input (the Source when it is not
//     connected), blurred by Blur Lens (3-box blur) and then scaled by 1 + blur / 24
//   - shift = Amount x 0.005 x W x W x slope of the lens (central difference), W the
//     project width in pixels; Fine divides by 100; Amount Rel X/Y scale each axis,
//     then Rotate Warp Dir turns it counterclockwise; rounded to 1/256 pixel
//   - output pixel p = Source at p + shift; Wrap X/Y: No (transparent), Tile, Reflect,
//     inside the Source minus the Crop margins
//   - Filter: where the warp shrinks the image, the Source is read from box-averaged
//     reduced copies (levels), blending two levels: level = fast log2 of how fast the
//     shift changes per pixel, per axis (both axes reduced, or only one)
//   - Mask: Post-effect mixes Source and result by the mask; Pre-effect uses the mask
//     moved by the same shift; Mask Lens multiplies the lens by the mask

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/blur.h"
#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "common/parallel.h"
#include "distort_atlas_glsl.h"
#include "distort_atlas_x_glsl.h"
#include "distort_lens_glsl.h"
#include "distort_shift_glsl.h"
#include "distort_warp_glsl.h"
#include "gpu/gpu.h"
#include "mask_glsl.h"

// Inputs: clip, label in the host, name, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", "source", 0.0f, 0, 0},
    {"Lens", "Lens", "lens", 0.0f, 1, 0},
    {"Mask", "Mask", "mask", 0.0f, 1, 1},
};
enum { SOURCE, LENS, MASK };

enum { WRAP_NO, WRAP_TILE, WRAP_REFLECT };

static const char *const apply[] = {"Post-effect", "Pre-effect"};
static const char *const wraps[] = {"No", "Tile", "Reflect"};
enum {
    APPLY_MASK,
    MASK_LENS,
    AMOUNT,
    FINE,
    BLUR_LENS,
    ROTATE,
    REL_X,
    REL_Y,
    WRAP_X,
    WRAP_Y,
    FILTER,
    BLUR_MASK,
    INVERT_MASK,
    MASK_USE,
    CROP_LEFT,
    CROP_RIGHT,
    CROP_TOP,
    CROP_BOTTOM,
    ENABLE_GPU,
};
static const MsParam params[] = {
    [APPLY_MASK] =
        {.type = MS_PARAM_CHOICE,
         .name = "apply_mask",
         .label = "Apply Mask",
         .hint =
             "Post-effect mixes the result with the Source by the Mask; Pre-effect moves the Mask "
             "with the image.",
         .entries = apply,
         .entry_count = 2},
    [MASK_LENS] = {.type = MS_PARAM_BOOL,
                   .name = "mask_lens",
                   .label = "Mask Lens",
                   .hint = "Multiply the Lens by the Mask."},
    [AMOUNT] = {.type = MS_PARAM_DOUBLE,
                .name = "amount",
                .label = "Amount",
                .hint = "Scales the amount of distortion.",
                .def = 1.0,
                .display_min = -2.0,
                .display_max = 2.0},
    [FINE] = {.type = MS_PARAM_BOOL,
              .name = "fine",
              .label = "Fine",
              .hint = "Divides the Amount by 100, for subtle distortions."},
    [BLUR_LENS] = {.type = MS_PARAM_WIDTH,
                   .name = "blur_lens",
                   .label = "Blur Lens",
                   .hint = "Blurs the Lens before computing its slopes.",
                   .def = 0.05,
                   .min = 0.0,
                   .max = INFINITY,
                   .display_max = 0.25},
    [ROTATE] = {.type = MS_PARAM_ANGLE,
                .name = "rotate_warp_dir",
                .label = "Rotate Warp Dir",
                .hint = "Rotates the direction of the displacement.",
                .display_min = -360.0,
                .display_max = 360.0},
    [REL_X] = {.type = MS_PARAM_DOUBLE,
               .name = "amount_rel_x",
               .label = "Amount Rel X",
               .hint = "Relative horizontal distortion.",
               .def = 1.0,
               .min = 0.0,
               .max = INFINITY,
               .display_max = 5.0},
    [REL_Y] = {.type = MS_PARAM_DOUBLE,
               .name = "amount_rel_y",
               .label = "Amount Rel Y",
               .hint = "Relative vertical distortion.",
               .def = 1.0,
               .min = 0.0,
               .max = INFINITY,
               .display_max = 5.0},
    [WRAP_X] = {.type = MS_PARAM_CHOICE,
                .name = "wrap_x",
                .label = "Wrap X",
                .hint = "What is shown when the warp reads beyond the left or right edge.",
                .def = WRAP_REFLECT,
                .entries = wraps,
                .entry_count = 3},
    [WRAP_Y] = {.type = MS_PARAM_CHOICE,
                .name = "wrap_y",
                .label = "Wrap Y",
                .hint = "What is shown when the warp reads beyond the top or bottom edge.",
                .def = WRAP_REFLECT,
                .entries = wraps,
                .entry_count = 3},
    [FILTER] = {.type = MS_PARAM_BOOL,
                .name = "filter",
                .label = "Filter",
                .hint = "Smooths the Source where the warp shrinks it, to avoid aliasing.",
                .def = 1},
    [BLUR_MASK] = {.type = MS_PARAM_WIDTH,
                   .name = "blur_mask",
                   .label = "Blur Mask",
                   .hint = "Blurs the Mask.",
                   .min = 0.0,
                   .max = INFINITY,
                   .display_max = 0.25},
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Apply the effect where the Mask is black."),
    [MASK_USE] = MS_PARAM_MASK_USE,
    [CROP_LEFT] = {.type = MS_PARAM_WIDTH,
                   .name = "crop_left",
                   .label = "Crop Left",
                   .hint = "Ignore this part of the Source on the left.",
                   .min = 0.0,
                   .max = 1.0,
                   .display_max = 1.0},
    [CROP_RIGHT] = {.type = MS_PARAM_WIDTH,
                    .name = "crop_right",
                    .label = "Crop Right",
                    .hint = "Ignore this part of the Source on the right.",
                    .min = 0.0,
                    .max = 1.0,
                    .display_max = 1.0},
    [CROP_TOP] = {.type = MS_PARAM_HEIGHT,
                  .name = "crop_top",
                  .label = "Crop Top",
                  .hint = "Ignore this part of the Source at the top.",
                  .min = 0.0,
                  .max = 1.0,
                  .display_max = 1.0},
    [CROP_BOTTOM] = {.type = MS_PARAM_HEIGHT,
                     .name = "crop_bottom",
                     .label = "Crop Bottom",
                     .hint = "Ignore this part of the Source at the bottom.",
                     .min = 0.0,
                     .max = 1.0,
                     .display_max = 1.0},
    [ENABLE_GPU] = {.type = MS_PARAM_BOOL,
                    .name = "enable_gpu",
                    .label = "Enable GPU",
                    .hint = "Render on the GPU; off renders on the CPU (same result).",
                    .def = 1},
};

#define MAX_LEVELS 16 // per axis: enough for 2^15 pixels

typedef struct {
    double amount; // shift in pixels per unit of lens slope (per pixel)
    double rel_x, rel_y;
    double cos_r, sin_r;                   // Rotate Warp Dir
    double blur_lens, blur_mask;           // pixels
    double crop_l, crop_r, crop_b, crop_t; // pixels
    int wrap_x, wrap_y;
    int filter;
    int has_lens, has_mask, mask_alpha, invert_mask, mask_lens, pre_effect;
    int gpu;
} Settings;

// A reduced copy of the Source: each pixel is the mean of a 2^nx x 2^ny block.
typedef struct {
    const float *data; // RGBA, bottom row first
    int width, height;
} Level;

// ---------------------------------------------------------------- sampling

// Level of detail for a footprint of d pixels: log2 with a linear step between powers
// of two (level n at d = 2^n), 0 below 1.
static double fast_log2(double d) {
    if (d <= 1.0)
        return 0.0;
    int e;
    const double m = frexp(d, &e); // d = m x 2^e, m in [0.5, 1)
    return (e - 1) + (2.0 * m - 1.0);
}

// Which Source pixels an axis reads, and what happens beyond them.
typedef struct {
    int mode;      // WRAP_NO, WRAP_TILE or WRAP_REFLECT
    double lo, hi; // the Source pixels left by the crop: [lo, hi)
} Axis;

// Brings coordinate s (pixel units, pixel i at i) inside the axis: Tile repeats the
// pixels, Reflect mirrors them about lo and hi. Returns 0 if nothing can be read there
// (Wrap No, more than a pixel outside).
static int wrap(double *s, const Axis *a) {
    const double size = a->hi - a->lo;
    if (size <= 0)
        return 0;
    if (a->mode == WRAP_NO)
        return *s > a->lo - 1 && *s < a->hi;
    double v;
    if (a->mode == WRAP_TILE) {
        v = a->lo + fmod(*s - a->lo, size);
        if (v < a->lo)
            v += size;
    } else {
        v = fmod(*s - a->lo, 2 * size);
        if (v < 0)
            v += 2 * size;
        v += a->lo;
        if (v > a->hi)
            v = 2 * a->hi - v;
        if (v > a->hi - 1)
            v = a->hi - 1;
    }
    *s = v;
    return 1;
}

// Pixel read for sample index i of a level `n` pixels long, where each pixel covers
// `scale` Source pixels; -1 for a transparent pixel (Wrap No).
static int tap(int i, int n, int scale, const Axis *a) {
    int lo = (int)ceil(a->lo / scale), hi = (int)floor(a->hi / scale);
    if (hi > n)
        hi = n;
    if (hi <= lo)
        hi = lo + 1;
    if (i >= lo && i < hi)
        return i;
    if (a->mode == WRAP_NO)
        return -1;
    if (a->mode == WRAP_TILE)
        return i < lo ? i + (hi - lo) : i - (hi - lo);
    return i < lo ? lo : hi - 1;
}

// Adds weight x the bilinear sample of level (nx, ny) at Source position (x, y) to out.
static void sample(const Level *lv, int nx, int ny, double x, double y, const Axis *ax,
                   const Axis *ay, double weight, double *out) {
    const double px = (x + 0.5) / (1 << nx) - 0.5, py = (y + 0.5) / (1 << ny) - 0.5;
    const double fx0 = floor(px), fy0 = floor(py);
    const double wx[2] = {1.0 - (px - fx0), px - fx0}, wy[2] = {1.0 - (py - fy0), py - fy0};
    for (int j = 0; j < 2; ++j) {
        const int ty = tap((int)fy0 + j, lv->height, 1 << ny, ay);
        if (ty < 0 || wy[j] == 0)
            continue;
        for (int i = 0; i < 2; ++i) {
            const int tx = tap((int)fx0 + i, lv->width, 1 << nx, ax);
            if (tx < 0 || wx[i] == 0)
                continue;
            const float *p = lv->data + ((size_t)ty * lv->width + tx) * 4;
            const double k = weight * wx[i] * wy[j];
            for (int c = 0; c < 4; ++c)
                out[c] += k * p[c];
        }
    }
}

// How fast the shift changes along one axis: the two components of its derivative
// combined as max + min / 2.
static double footprint(double a, double b) {
    a = fabs(a);
    b = fabs(b);
    return a > b ? a + 0.5 * b : b + 0.5 * a;
}

// ---------------------------------------------------------------- warp

typedef struct {
    const Settings *s;
    const float *source;
    const float *mask; // NULL without a mask
    const float *sx;
    const float *sy;
    const float *lod;       // level of detail per pixel, x then y; NULL without Filter
    const Level *levels;    // [ny * MAX_LEVELS + nx]
    int levels_x, levels_y; // levels available per axis
    int width, height;
    float *output;
} Warp;

static void warp_rows(int begin, int end, void *user) {
    const Warp *w = user;
    const Settings *s = w->s;
    const int width = w->width, height = w->height;
    for (int y = begin; y < end; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (size_t)y * width + x;
            const Axis ax = {s->wrap_x, s->crop_l, width - s->crop_r};
            const Axis ay = {s->wrap_y, s->crop_b, height - s->crop_t};
            double px = (double)x + w->sx[i], py = (double)y + w->sy[i];
            double rgba[4] = {0, 0, 0, 0};
            const int inside = wrap(&px, &ax) & wrap(&py, &ay);
            if (inside && !w->lod) {
                const Level full = {w->source, width, height};
                sample(&full, 0, 0, px, py, &ax, &ay, 1.0, rgba);
            } else if (inside) {
                double lx = w->lod[i * 2], ly = w->lod[i * 2 + 1];
                if (lx > w->levels_x - 1)
                    lx = w->levels_x - 1;
                if (ly > w->levels_y - 1)
                    ly = w->levels_y - 1;
                // Blend the levels below and above, by the fraction of the larger level.
                const int nx0 = (int)floor(lx), ny0 = (int)floor(ly);
                const int nx1 = (int)ceil(lx), ny1 = (int)ceil(ly);
                const double f = lx >= ly ? lx - nx0 : ly - ny0;
                sample(&w->levels[ny0 * MAX_LEVELS + nx0], nx0, ny0, px, py, &ax, &ay, 1.0 - f,
                       rgba);
                if (f > 0)
                    sample(&w->levels[ny1 * MAX_LEVELS + nx1], nx1, ny1, px, py, &ax, &ay, f, rgba);
            }
            float *out = w->output + i * 4;
            const float *src = w->source + i * 4;
            double m = 1.0;
            if (w->mask && s->pre_effect) {
                // The mask moves with the image: read it at the shifted position.
                const Axis mx_axis = {s->wrap_x == WRAP_NO ? WRAP_REFLECT : s->wrap_x, 0, width};
                const Axis my_axis = {s->wrap_y == WRAP_NO ? WRAP_REFLECT : s->wrap_y, 0, height};
                double mx = (double)x + w->sx[i], my = (double)y + w->sy[i];
                m = 0.0;
                if (wrap(&mx, &mx_axis) & wrap(&my, &my_axis)) {
                    const int x0 = (int)floor(mx), y0 = (int)floor(my);
                    const int x1 = x0 + 1 < width ? x0 + 1 : x0, y1 = y0 + 1 < height ? y0 + 1 : y0;
                    const double fx = mx - x0, fy = my - y0;
                    const float *k = w->mask;
                    m = (k[(size_t)y0 * width + x0] * (1 - fx) + k[(size_t)y0 * width + x1] * fx) *
                            (1 - fy) +
                        (k[(size_t)y1 * width + x0] * (1 - fx) + k[(size_t)y1 * width + x1] * fx) *
                            fy;
                }
            } else if (w->mask) {
                m = w->mask[i];
            }
            for (int c = 0; c < 4; ++c)
                out[c] = (float)(src[c] + (rgba[c] - src[c]) * m);
        }
    }
}

// ---------------------------------------------------------------- processing

typedef struct {
    const float *sx;
    const float *sy;
    int width, height;
    float *lod;
} Lod;

// Level of detail of each pixel along x and y, from the derivatives of the shift
// (central differences, one-sided at the borders).
static void lod_rows(int begin, int end, void *user) {
    const Lod *d = user;
    const int width = d->width, height = d->height;
    for (int y = begin; y < end; ++y) {
        for (int x = 0; x < width; ++x) {
            // Neighbours: left, right, below, above (the pixel itself at a border).
            const int xl = x > 0 ? x - 1 : x, xr = x < width - 1 ? x + 1 : x;
            const int yb = y > 0 ? y - 1 : y, yt = y < height - 1 ? y + 1 : y;
            const size_t i = (size_t)y * width + x;
            const size_t l = (size_t)y * width + xl, r = (size_t)y * width + xr;
            const size_t b = (size_t)yb * width + x, t = (size_t)yt * width + x;
            const double hx = xr - xl, hy = yt - yb;
            d->lod[i * 2] =
                (float)fast_log2(footprint((d->sx[r] - d->sx[l]) / hx, (d->sy[r] - d->sy[l]) / hx));
            d->lod[i * 2 + 1] =
                (float)fast_log2(footprint((d->sx[t] - d->sx[b]) / hy, (d->sy[t] - d->sy[b]) / hy));
        }
    }
}

// Builds the levels up to (levels_x - 1, levels_y - 1) in `store`, one after the
// other; level (0, 0), the Source itself, must be set already. Each level halves the
// one to its left (or, in the first column, the one below) and drops an odd last row
// or column.
static void build_levels(int levels_x, int levels_y, float *store, Level *levels) {
    for (int ny = 0; ny < levels_y; ++ny) {
        for (int nx = 0; nx < levels_x; ++nx) {
            Level *lv = &levels[ny * MAX_LEVELS + nx];
            if (nx == 0 && ny == 0)
                continue;
            const int along_x = nx > 0;
            const Level *from = along_x ? &levels[(size_t)ny * MAX_LEVELS + nx - 1]
                                        : &levels[(size_t)(ny - 1) * MAX_LEVELS];
            const int w = along_x ? from->width / 2 : from->width,
                      h = along_x ? from->height : from->height / 2;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float *a = from->data + ((size_t)(along_x ? y : 2 * y) * from->width +
                                                   (size_t)(along_x ? 2 * x : x)) *
                                                      4;
                    const float *b = along_x ? a + 4 : a + (size_t)from->width * 4;
                    for (int c = 0; c < 4; ++c)
                        store[((size_t)y * w + x) * 4 + c] = 0.5f * (a[c] + b[c]);
                }
            }
            *lv = (Level){store, w, h};
            store += (size_t)w * h * 4;
        }
    }
}

// Number of levels along an axis of n pixels (down to 1 pixel).
static int max_levels(int n) {
    int levels = 1;
    while (levels < MAX_LEVELS && (n >> levels) >= 1)
        ++levels;
    return levels;
}

static int process(const MsInput *inputs, int width, int height, float *output, char *err,
                   size_t err_size, const Settings *s) {
    const size_t pixels = (size_t)width * height;
    const float *source = inputs[SOURCE].pixels;
    const float *lens_input = s->has_lens ? inputs[LENS].pixels : source;

    float *mask = s->has_mask ? malloc(pixels * sizeof(float)) : NULL;
    float *lens = malloc(pixels * sizeof(float));
    float *sx = malloc(pixels * sizeof(float));
    float *sy = malloc(pixels * sizeof(float));
    float *lod = s->filter ? malloc(pixels * 2 * sizeof(float)) : NULL;
    float *store = NULL; // the Filter levels
    int ok = lens && sx && sy && (mask || !s->has_mask) && (lod || !s->filter);

    // 1. Mask: luma or alpha, blurred by Blur Mask.
    if (ok && mask) {
        ms_mask_values(inputs[MASK].pixels, pixels, s->mask_alpha, s->invert_mask, mask);
        ms_box3_blur(mask, width, height, 1, s->blur_mask);
    }

    // 2. Lens: luma (masked with Mask Lens), blurred by Blur Lens.
    if (ok) {
        const int masked = mask && s->mask_lens;
        for (size_t i = 0; i < pixels; ++i)
            lens[i] = ms_luma(lens_input + i * 4) * (masked ? mask[i] : 1.0f);
        if (s->blur_lens > 0) {
            ms_box3_blur(lens, width, height, 1, s->blur_lens);
            const double gain = 1.0 + s->blur_lens / 24.0; // a blurred lens warps more
            for (size_t i = 0; i < pixels; ++i)
                lens[i] = (float)(lens[i] * gain);
        }
    }

    // 3. Shift per pixel: slope of the lens (central difference), rounded to 1/256 pixel.
    // NOLINTBEGIN(clang-analyzer-security.ArrayBound): false positive, the analyzer
    // does not tie the buffer size (width x height) to the loop bounds.
    if (ok) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                // Neighbours: left, right, below, above (the pixel itself at a border).
                const int xl = x > 0 ? x - 1 : x, xr = x < width - 1 ? x + 1 : x;
                const int yb = y > 0 ? y - 1 : y, yt = y < height - 1 ? y + 1 : y;
                const size_t i = (size_t)y * width + x;
                const float *row = lens + (size_t)y * width;
                const double gx = xr > xl ? (row[xr] - row[xl]) / (double)(xr - xl) : 0.0;
                const double gy =
                    yt > yb ? (lens[(size_t)yt * width + x] - lens[(size_t)yb * width + x]) /
                                  (double)(yt - yb)
                            : 0.0;
                const double vx = s->amount * s->rel_x * gx, vy = s->amount * s->rel_y * gy;
                sx[i] = (float)(floor((s->cos_r * vx - s->sin_r * vy) * 256.0 + 0.5) / 256.0);
                sy[i] = (float)(floor((s->sin_r * vx + s->cos_r * vy) * 256.0 + 0.5) / 256.0);
            }
        }
    }
    // NOLINTEND(clang-analyzer-security.ArrayBound)

    // 4. Filter: the reduced copies of the Source the warp needs.
    Level levels[MAX_LEVELS * MAX_LEVELS];
    int levels_x = 1, levels_y = 1;
    if (ok && s->filter) {
        Lod d = {sx, sy, width, height, lod};
        ms_parallel(height, lod_rows, &d);
        // Coarsest level needed per axis, capped by the image size.
        double top_x = 0, top_y = 0;
        for (size_t i = 0; i < pixels; ++i) {
            if (lod[i * 2] > top_x)
                top_x = lod[i * 2];
            if (lod[i * 2 + 1] > top_y)
                top_y = lod[i * 2 + 1];
        }
        levels_x = (int)ceil(top_x) + 1;
        levels_y = (int)ceil(top_y) + 1;
        if (levels_x > max_levels(width))
            levels_x = max_levels(width);
        if (levels_y > max_levels(height))
            levels_y = max_levels(height);
        size_t size = 0;
        for (int ny = 0; ny < levels_y; ++ny)
            for (int nx = 0; nx < levels_x; ++nx)
                if (nx || ny)
                    size += (size_t)(width >> nx) * (height >> ny) * 4;
        levels[0] = (Level){source, width, height};
        if (size > 0) {
            store = malloc(size * sizeof(float));
            ok = store != NULL;
            if (ok)
                build_levels(levels_x, levels_y, store, levels);
        }
    }

    // 5. Warp.
    if (ok) {
        Warp w = {s, source, mask, sx, sy, lod, levels, levels_x, levels_y, width, height, output};
        ms_parallel(height, warp_rows, &w);
    } else {
        snprintf(err, err_size, "not enough memory");
    }
    free(store);
    free(mask);
    free(lens);
    free(sx);
    free(sy);
    free(lod);
    return ok;
}

// ---------------------------------------------------------------- GPU

// Same as `process`, as shader passes:
//   1 mask value, 2-7 blur of the mask, 8 lens, 9-14 blur of the lens, 15 shift,
//   16-17 Filter levels (all of them, side by side in one image), 18 warp.
static int process_gpu(const MsInput *inputs, int width, int height, float *output, char *err,
                       size_t err_size, const Settings *s) {
    const int levels_x = s->filter ? max_levels(width) : 1,
              levels_y = s->filter ? max_levels(height) : 1;
    int atlas_w = 0, atlas_h = 0;
    for (int n = 0; n < levels_x; ++n)
        atlas_w += width >> n;
    for (int n = 0; n < levels_y; ++n)
        atlas_h += height >> n;

    const double blur_mask = s->has_mask ? s->blur_mask : 0.0;
    MsPass passes[18];
    MsShaderValue radius[12];
    int n = 0;
    passes[n++] = (MsPass){mask_glsl, NULL, NULL, 0, 0, 0};
    n = ms_add_blur(passes, n, radius, NULL, blur_mask, blur_mask);
    passes[n++] = (MsPass){distort_lens_glsl, NULL, NULL, 0, 0, 0};
    n = ms_add_blur(passes, n, radius + 6, NULL, s->blur_lens, s->blur_lens);
    passes[n++] = (MsPass){distort_shift_glsl, NULL, NULL, 0, 0, 0};
    passes[n++] = (MsPass){distort_atlas_x_glsl, "source", NULL, 0, atlas_w, height};
    passes[n++] = (MsPass){distort_atlas_glsl, NULL, NULL, 0, atlas_w, atlas_h};
    passes[n++] = (MsPass){distort_warp_glsl, "source", NULL, 0, 0, 0};

    const double gain = s->blur_lens > 0 ? 1.0 + s->blur_lens / 24.0 : 1.0;
    const MsShaderValue shader_values[] = {
        {"has_lens", (float)s->has_lens},
        {"has_mask", (float)s->has_mask},
        {"mask_lens", (float)s->mask_lens},
        {"mask_alpha", (float)s->mask_alpha},
        {"invert_mask", (float)s->invert_mask},
        {"pre_effect", (float)s->pre_effect},
        {"amount", (float)(s->amount * gain)},
        {"rel_x", (float)s->rel_x},
        {"rel_y", (float)s->rel_y},
        {"cos_r", (float)s->cos_r},
        {"sin_r", (float)s->sin_r},
        {"source_height", (float)height},
        {"wrap_x", (float)s->wrap_x},
        {"wrap_y", (float)s->wrap_y},
        {"crop_l", (float)s->crop_l},
        {"crop_r", (float)s->crop_r},
        {"crop_b", (float)s->crop_b},
        {"crop_t", (float)s->crop_t},
        {"filter_on", (float)s->filter},
        {"levels_x", (float)levels_x},
        {"levels_y", (float)levels_y},
    };
    return ms_gpu_run(passes, n, inputs, ARRAY_SIZE(clips), width, height, shader_values,
                      ARRAY_SIZE(shader_values), output, err, err_size);
}

// ---------------------------------------------------------------- render

static int render(const MsRender *r) {
    const double *v = r->values;
    const double w = r->frame_width; // a width of 1.0 in pixels
    Settings s;
    // Shift = Amount x 0.005 x W x (lens slope per pixel x W).
    s.amount = v[AMOUNT] * 0.005 * w * w;
    if (v[FINE])
        s.amount /= 100.0;
    s.rel_x = v[REL_X];
    s.rel_y = v[REL_Y];
    const double angle = v[ROTATE] * M_PI / 180.0;
    s.cos_r = cos(angle);
    s.sin_r = sin(angle);
    s.blur_lens = v[BLUR_LENS];
    s.blur_mask = v[BLUR_MASK];
    s.crop_l = v[CROP_LEFT];
    s.crop_r = v[CROP_RIGHT];
    s.crop_b = v[CROP_BOTTOM];
    s.crop_t = v[CROP_TOP];
    s.wrap_x = (int)v[WRAP_X];
    s.wrap_y = (int)v[WRAP_Y];
    s.filter = (int)v[FILTER];
    s.has_lens = r->connected[LENS];
    s.has_mask = r->connected[MASK];
    s.mask_alpha = (int)v[MASK_USE];
    s.invert_mask = (int)v[INVERT_MASK];
    s.mask_lens = (int)v[MASK_LENS];
    s.pre_effect = v[APPLY_MASK] == 1;
    s.gpu = (int)v[ENABLE_GPU];
    return (s.gpu ? process_gpu : process)(r->inputs, r->width, r->height, r->output, r->err,
                                           r->err_size, &s);
}

const MsEffect ms_distort_effect = {
    MS_ID("Distort"),
    "MS_Distort",
    "Warps the Source by the slopes of the Lens input, like looking through textured glass.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
};
