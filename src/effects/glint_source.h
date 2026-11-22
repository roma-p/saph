// What Glint's rays and Glare's spots come from: the parts of the Source above the
// threshold, through the Mask (src/halide/glint_generator.cpp's glint_source kernel).

#pragma once

#include "effects/pixels.h"
#include "engine/engine.h"

typedef struct {
    int has_mask, invert_mask, mask_type, blur; // Mask Type: Luma, Color, Alpha
    float blur_frac, threshold, add[3], from_alpha;
} MsGlintSource;

// From the settings: Threshold, Threshold Add Color (`add`, three), Threshold Blur in pixels,
// From Alpha, Mask Type, Invert Mask, with or without a Mask.
static inline MsGlintSource ms_glint_source_of(double threshold, const double *add, double blur,
                                               double from_alpha, double mask_type,
                                               double invert_mask, int has_mask) {
    MsGlintSource g = {.has_mask = has_mask,
                       .invert_mask = (int)invert_mask,
                       .mask_type = (int)mask_type,
                       .threshold = (float)threshold,
                       .add = {(float)add[0], (float)add[1], (float)add[2]},
                       .from_alpha = (float)from_alpha};
    g.blur = ms_whole_pixels(blur, 1, &g.blur_frac);
    return g;
}

// `glints` from `source` and `mask` on `api`. Returns Halide's status.
static inline int ms_glint_source(MsApi api, const MsGlintSource *g, MsImage *source, MsImage *mask,
                                  MsImage *glints) {
    return MS_KERNEL(MS_K_GLINT_SOURCE, api, MS_IMAGE("source", source), MS_IMAGE("mask", mask),
                     MS_INT("has_mask", g->has_mask), MS_INT("invert_mask", g->invert_mask),
                     MS_INT("mask_type", g->mask_type), MS_FLOAT("from_alpha", g->from_alpha),
                     MS_INT("blur", g->blur), MS_FLOAT("blur_frac", g->blur_frac),
                     MS_FLOAT("threshold", g->threshold), MS_FLOAT("add_r", g->add[0]),
                     MS_FLOAT("add_g", g->add[1]), MS_FLOAT("add_b", g->add[2]),
                     MS_IMAGE("glints", glints));
}
