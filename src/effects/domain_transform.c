// Domain Transform filtering on the CPU. See domain_transform.h.
//
// Each pass filters every row (horizontal), then every column (vertical), one line at
// a time; lines are spread over the CPU cores. Along a line, sample i sits at position
// ct[i] = sum of the distances so far: an edge is a big jump in ct, so it separates
// the samples on both sides.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/parallel.h"
#include "effects/domain_transform.h"

#define EPS 1e-9 // window bounds are inclusive

// ---------------------------------------------------------------- distances

static double contrast(const float *p, const float *q, int use_alpha) {
    const double rgb = fabs(p[0] - q[0]) + fabs(p[1] - q[1]) + fabs(p[2] - q[2]);
    return use_alpha ? 0.75 * (rgb + fabs(p[3] - q[3])) : rgb;
}

static double stretch(const float *mask, size_t i, size_t j, double floor_value) {
    if (!mask)
        return 1.0;
    const double m = mask[i] < mask[j] ? mask[i] : mask[j];
    return 1.0 / (m > floor_value ? m : floor_value);
}

void ms_dt_distances(const float *edge, int width, int height, double scale_x, double scale_y,
                     int use_alpha, const float *mask, double mask_floor_x, double mask_floor_y,
                     float *dx, float *dy) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (size_t)y * width + x;
            const float *p = edge + i * 4;
            dx[i] = x == 0 ? 1.0f
                           : (float)(stretch(mask, i, i - 1, mask_floor_x) +
                                     scale_x * contrast(p, p - 4, use_alpha));
            dy[i] = y == 0 ? 1.0f
                           : (float)(stretch(mask, i, i - width, mask_floor_y) +
                                     scale_y * contrast(p, p - (size_t)width * 4, use_alpha));
        }
    }
}

// ---------------------------------------------------------------- one line
// v: n samples x 4 channels (double), d: n distances (d[0] unused).

// Recursive filter: forward then backward, weight a^d to the neighbour.
static void line_soft(double *v, const double *d, int n, double sigma, double *w) {
    if (n < 2)
        return;
    const double a = exp(-sqrt(2.0) / sigma);
    for (int i = 1; i < n; ++i)
        w[i] = pow(a, d[i]);
    for (int i = 1; i < n; ++i)
        for (int c = 0; c < 4; ++c)
            v[i * 4 + c] = (1 - w[i]) * v[i * 4 + c] + w[i] * v[(i - 1) * 4 + c];
    for (int i = n - 2; i >= 0; --i)
        for (int c = 0; c < 4; ++c)
            v[i * 4 + c] = (1 - w[i + 1]) * v[i * 4 + c] + w[i + 1] * v[(i + 1) * 4 + c];
}

// Mean of the samples whose position is in [ct[i] - below, ct[i] + above], for every i,
// added to out with weight `weight`. prefix[j] = sum of the first j samples.
static void window_mean(const double *ct, const double *prefix, int n, double below, double above,
                        double weight, double *out) {
    int lo = 0, hi = 0;
    for (int i = 0; i < n; ++i) {
        while (ct[lo] < ct[i] - below - EPS)
            ++lo;
        if (hi < i)
            hi = i;
        while (hi + 1 < n && ct[hi + 1] <= ct[i] + above + EPS)
            ++hi;
        const double count = hi - lo + 1;
        for (int c = 0; c < 4; ++c)
            out[i * 4 + c] += weight * (prefix[(hi + 1) * 4 + c] - prefix[lo * 4 + c]) / count;
    }
}

// Normalised convolution: box of radius r in the transformed domain. A fractional
// radius blends the box [-n, n] with the box extended by one sample below: the
// Subpixel option (it shifts the result slightly towards lower coordinates).
static void line_sharp(double *v, const double *d, int n, double r, double *ct, double *prefix,
                       double *out) {
    ct[0] = 0;
    for (int i = 1; i < n; ++i)
        ct[i] = ct[i - 1] + d[i];
    memset(prefix, 0, 4 * sizeof(double));
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 4; ++c)
            prefix[(i + 1) * 4 + c] = prefix[i * 4 + c] + v[i * 4 + c];
    memset(out, 0, (size_t)n * 4 * sizeof(double));
    const double whole = floor(r), fraction = r - whole;
    window_mean(ct, prefix, n, whole, whole, 1.0 - fraction, out);
    if (fraction > 0)
        window_mean(ct, prefix, n, whole + 1, whole, fraction, out);
    memcpy(v, out, (size_t)n * 4 * sizeof(double));
}

// Interpolated convolution: mean over [ct - h, ct + h] of the signal linearly
// interpolated between samples (constant beyond the first and last sample).
static void line_softer(double *v, const double *d, int n, double h, double *ct, double *area,
                        double *out) {
    ct[0] = 0;
    for (int i = 1; i < n; ++i)
        ct[i] = ct[i - 1] + d[i];
    memset(area, 0, 4 * sizeof(double)); // area[i] = integral from ct[0] to ct[i]
    for (int i = 1; i < n; ++i)
        for (int c = 0; c < 4; ++c)
            area[i * 4 + c] =
                area[(i - 1) * 4 + c] + d[i] * 0.5 * (v[i * 4 + c] + v[(i - 1) * 4 + c]);
    int j_lo = 0, j_hi = 0; // segments holding ct[i] - h and ct[i] + h
    for (int i = 0; i < n; ++i) {
        double at[2][4];
        for (int side = 0; side < 2; ++side) {
            const double t = side ? ct[i] + h : ct[i] - h;
            int *j = side ? &j_hi : &j_lo;
            while (*j + 1 < n - 1 && ct[*j + 1] <= t)
                ++*j;
            for (int c = 0; c < 4; ++c) {
                if (n == 1 || t <= ct[0]) {
                    at[side][c] = (t - ct[0]) * v[c];
                } else if (t >= ct[n - 1]) {
                    at[side][c] = area[(n - 1) * 4 + c] + (t - ct[n - 1]) * v[(n - 1) * 4 + c];
                } else {
                    const int k = *j;
                    const double u = t - ct[k], len = ct[k + 1] - ct[k];
                    const double v0 = v[k * 4 + c], v1 = v[(k + 1) * 4 + c];
                    at[side][c] = area[k * 4 + c] + u * v0 + 0.5 * u * u * (v1 - v0) / len;
                }
            }
        }
        for (int c = 0; c < 4; ++c)
            out[i * 4 + c] = (at[1][c] - at[0][c]) / (2 * h);
    }
    memcpy(v, out, (size_t)n * 4 * sizeof(double));
}

// ---------------------------------------------------------------- passes over lines

typedef struct {
    float *image;
    const float *dist;
    int width, height;
    int vertical; // 0: lines are rows, 1: lines are columns
    MsDtType type;
    double size; // sigma (Soft) or radius (Sharp, Softer)
} Pass;

static void filter_lines(int begin, int end, void *user) {
    const Pass *p = user;
    const int n = p->vertical ? p->height : p->width;
    const size_t step = p->vertical ? (size_t)p->width : 1; // pixels between samples
    // Scratch for one line, reused for every line of this slice.
    double *v = malloc((size_t)n * 4 * sizeof(double));
    double *d = malloc((size_t)n * sizeof(double));
    double *ct = malloc((size_t)n * sizeof(double));
    double *acc = malloc((size_t)(n + 1) * 4 * sizeof(double));
    double *out = malloc((size_t)n * 4 * sizeof(double));
    if (v && d && ct && acc && out) {
        for (int line = begin; line < end; ++line) {
            const size_t first = p->vertical ? (size_t)line : (size_t)line * p->width;
            for (int i = 0; i < n; ++i) {
                const size_t px = first + i * step;
                for (int c = 0; c < 4; ++c)
                    v[i * 4 + c] = p->image[px * 4 + c];
                d[i] = p->dist[px];
            }
            if (p->type == MS_DT_SOFT)
                line_soft(v, d, n, p->size, ct);
            else if (p->type == MS_DT_SHARP)
                line_sharp(v, d, n, p->size, ct, acc, out);
            else
                line_softer(v, d, n, p->size, ct, acc, out);
            for (int i = 0; i < n; ++i) {
                const size_t px = first + i * step;
                for (int c = 0; c < 4; ++c)
                    p->image[px * 4 + c] = (float)v[i * 4 + c];
            }
        }
    }
    free(v);
    free(d);
    free(ct);
    free(acc);
    free(out);
}

int ms_dt_filter(float *image, int width, int height, const float *dx, const float *dy,
                 const MsDtSettings *s) {
    const int n = s->passes < 1 ? 1 : s->passes;
    int ok = 1;
    for (int pass = 0; pass < n; ++pass) {
        for (int vertical = 0; vertical < 2; ++vertical) {
            const double blur = vertical ? s->blur_y : s->blur_x;
            if (blur <= 0)
                continue;
            // Width per pass: radius (b + N) / N, sigma half of it.
            const double size = s->type == MS_DT_SOFT ? (blur + n) / (2.0 * n) : (blur + n) / n;
            Pass p = {image, vertical ? dy : dx, width, height, vertical, s->type, size};
            ok &= ms_parallel(vertical ? width : height, filter_lines, &p);
        }
    }
    return ok;
}
