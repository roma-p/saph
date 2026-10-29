// Mask input helpers. See mask.h.

#include "common/mask.h"

const char *const ms_mask_use_entries[2] = {"Luma", "Alpha"};

void ms_mask_values(const float *rgba, size_t pixels, int alpha, int invert, float *values) {
    for (size_t i = 0; i < pixels; ++i) {
        const float *p = rgba + i * 4;
        const float v = alpha ? p[3] : ms_luma(p);
        values[i] = invert ? 1.0f - v : v;
    }
}
