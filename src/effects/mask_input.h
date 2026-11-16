// The mask value of the effects with a Mask input (src/halide/mask_generator.cpp): made on
// the GPU from the Mask (its luma or alpha, inverted, blurred), or one value of 1 for every
// pixel (strides 0) without one.

#pragma once

#include "common/effect.h"
#include "engine/engine.h"

typedef struct {
    MsImage mask;   // the Mask input as the host gives it
    MsImage value;  // the mask value
    MsImage *input; // what to upload: the Mask (the Source when the host plugs the Source in
                    // both: uploaded once), or the one value
    float *pixels, one;
    int has_mask, alpha, invert, blur; // Mask Use (1: alpha), Invert Mask, Blur Mask (pixels)
} MsMask;

// Sets `m` up from input `clip` of `r`, the Source being input `source_clip` as `source`.
// Returns 1, or 0 when out of memory (the reason in r->err). `m` must not move until freed
// (its images point into it).
int ms_mask_init(MsMask *m, const MsRender *r, int clip, int source_clip, MsImage *source,
                 double mask_use, double invert_mask, double blur_mask);

// The mask value from the Mask, on `api` (nothing without a Mask). Returns Halide's status.
int ms_mask_run(MsMask *m, MsApi api);

void ms_mask_free(MsMask *m);
