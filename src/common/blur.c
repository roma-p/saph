// The 3-box blur. See blur.h.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "box_h_glsl.h"
#include "box_v_glsl.h"
#include "common/blur.h"
#include "common/parallel.h"

void ms_box_radii(double radius, int radii[3]) {
    const int total = (int)floor(radius + 0.5);
    for (int i = 0; i < 3; ++i)
        radii[i] = total / 3 + (i >= 3 - total % 3 ? 1 : 0);
}

// ---------------------------------------------------------------- CPU

typedef struct {
    float *image;
    int width, height, channels, vertical;
    int radii[3];
} BoxPass;

// Mirrored border (pixel -1 = pixel 0), repeated for a box wider than the line.
static int mirror(int i, int n) {
    i %= 2 * n;
    if (i < 0)
        i += 2 * n;
    return i < n ? i : 2 * n - 1 - i;
}

static void box_lines(int begin, int end, void *user) {
    const BoxPass *p = user;
    const int n = p->vertical ? p->height : p->width, ch = p->channels;
    const size_t step = p->vertical ? (size_t)p->width : 1;
    double *v = malloc((size_t)n * ch * sizeof(double));
    double *prefix = malloc((size_t)(n + 1) * ch * sizeof(double));
    if (v && prefix) {
        for (int line = begin; line < end; ++line) {
            const size_t first = p->vertical ? (size_t)line : (size_t)line * p->width;
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < ch; ++c)
                    v[i * ch + c] = p->image[(first + i * step) * ch + c];
            for (int b = 0; b < 3; ++b) {
                const int r = p->radii[b];
                if (r <= 0)
                    continue;
                memset(prefix, 0, ch * sizeof(double));
                for (int i = 0; i < n; ++i)
                    for (int c = 0; c < ch; ++c)
                        prefix[(i + 1) * ch + c] = prefix[i * ch + c] + v[i * ch + c];
                for (int i = 0; i < n; ++i) {
                    for (int c = 0; c < ch; ++c) {
                        double sum = 0;
                        if (i - r >= 0 && i + r < n) {
                            sum = prefix[(i + r + 1) * ch + c] - prefix[(i - r) * ch + c];
                        } else {
                            for (int k = i - r; k <= i + r; ++k)
                                sum += prefix[(mirror(k, n) + 1) * ch + c] -
                                       prefix[mirror(k, n) * ch + c];
                        }
                        v[i * ch + c] = sum / (2 * r + 1); // in place: the sums come from `prefix`
                    }
                }
            }
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < ch; ++c)
                    p->image[(first + i * step) * ch + c] = (float)v[i * ch + c];
        }
    }
    free(v);
    free(prefix);
}

void ms_box3_blur(float *image, int width, int height, int channels, double radius) {
    BoxPass p = {image, width, height, channels, 0, {0, 0, 0}};
    ms_box_radii(radius, p.radii);
    if (p.radii[2] <= 0)
        return; // the largest one
    ms_parallel(height, box_lines, &p);
    p.vertical = 1;
    ms_parallel(width, box_lines, &p);
}

// ---------------------------------------------------------------- GPU

int ms_add_blur(MsPass *passes, int count, MsShaderValue radius[6], const char *from,
                double radius_x, double radius_y) {
    int rx[3], ry[3];
    ms_box_radii(radius_x, rx);
    ms_box_radii(radius_y, ry);
    for (int i = 0; i < 6; ++i) {
        radius[i] = (MsShaderValue){"radius", (float)(i < 3 ? rx[i] : ry[i - 3])};
        passes[count + i] =
            (MsPass){i < 3 ? box_h_glsl : box_v_glsl, i == 0 ? from : NULL, &radius[i], 1, 0, 0};
    }
    return count + 6;
}
