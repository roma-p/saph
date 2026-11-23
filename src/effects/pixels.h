// Widths in pixels for the kernels.

#pragma once

#include <math.h>

// A width `t` in pixels (kept in 0 to 1e7) as whole pixels; with `subpixel` the fraction past
// them in *frac, else rounded (*frac 0).
static inline int ms_whole_pixels(double t, int subpixel, float *frac) {
    t = t > 0 ? (t < 1e7 ? t : 1e7) : 0;
    const double whole = subpixel ? floor(t) : floor(t + 0.5);
    *frac = (float)(subpixel ? t - whole : 0.0);
    return (int)whole;
}
