// The blur the effects use: 3 box blurs in a row, horizontal then vertical, borders
// mirrored (pixel -1 = pixel 0). The total radius round(radius) is
// split into 3 near-equal box radii. On the CPU, and as GPU passes.

#pragma once

#include "gpu/gpu.h"

// The 3 box radii of a blur of total radius round(radius).
void ms_box_radii(double radius, int radii[3]);

// Blurs `image` in place on all cores: width x height pixels of `channels` floats,
// packed.
void ms_box3_blur(float *image, int width, int height, int channels, double radius);

// Adds the 6 passes of a blur (3 horizontal, 3 vertical: box_h.glsl, box_v.glsl) at
// passes[count], the first one reading `from` (NULL: the pass before). `radius` gets
// the 6 radius values, so it must live until the passes run. Returns the new count.
int ms_add_blur(MsPass *passes, int count, MsShaderValue radius[6], const char *from,
                double radius_x, double radius_y);
