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
    float one;
    int has_mask, alpha, invert, blur; // Mask Use (1: alpha), Invert Mask, Blur Mask (pixels)
} MsMask;

// The floats of the mask value's host memory (0 without a Mask): a part of the render's
// scratch memory (ms_parts_size).
size_t ms_mask_floats(const MsRender *r, int clip);

// Sets `m` up from input `clip` of `r`, the Source being input `source_clip` as `source`, the
// mask value in `pixels` (ms_mask_floats of them). `m` must not move (its images point into
// it).
void ms_mask_init(MsMask *m, const MsRender *r, int clip, int source_clip, MsImage *source,
                  float *pixels, double mask_use, double invert_mask, double blur_mask);

// The mask value as an image between the kernels (ms_run's `between`), NULL without a Mask
// (the one value is an input then).
static inline MsImage *ms_mask_between(MsMask *m) { return m->has_mask ? &m->value : NULL; }

// The mask value from the Mask, on `api` (nothing without a Mask). Returns Halide's status.
int ms_mask_run(MsMask *m, MsApi api);
