// A render's inputs and output as images for the kernels.

#pragma once

#include "common/effect.h"
#include "engine/engine.h"

// Input `clip` of `r` (the render's size).
static inline void ms_input_image(MsImage *image, const MsRender *r, int clip) {
    ms_image_rgba(image, r->inputs[clip].pixels, r->inputs[clip].row, r->half, r->width, r->height);
}

// The output of `r`.
static inline void ms_output_image(MsImage *image, const MsRender *r) {
    ms_image_rgba(image, r->output, r->output_row, r->half, r->width, r->height);
}

// What the kernels read for input `clip`: `image` (set up from it), or `source` (input
// `source_clip`) when it is not `used` (e.g. not connected) or is the Source's image itself in
// the host (uploaded once).
static inline MsImage *ms_input_or(MsImage *image, const MsRender *r, int clip, int used,
                                   MsImage *source, int source_clip) {
    ms_input_image(image, r, clip);
    return used && !ms_same_input(&r->inputs[clip], &r->inputs[source_clip]) ? image : source;
}
