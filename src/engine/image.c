// Images for the kernels. See engine.h.

#include <string.h>

#include "engine/engine.h"

void ms_image(MsImage *image, const float *pixels, ptrdiff_t row, int width, int height,
              int channels) {
    memset(image, 0, sizeof *image);
    image->dim[0] = (halide_dimension_t){0, width, channels, 0};
    image->dim[1] = (halide_dimension_t){0, height, (int32_t)row, 0};
    image->dim[2] = (halide_dimension_t){0, channels, 1, 0};
    image->b.host = (uint8_t *)pixels;
    image->b.type.code = halide_type_float;
    image->b.type.bits = 32;
#if HALIDE_VERSION_MAJOR < 22
    image->b.type.lanes = 1; // Halide 22 replaced it with `info`, 0 here
#endif
    image->b.dimensions = channels > 1 ? 3 : 2;
    image->b.dim = image->dim;
}

void ms_image_constant(MsImage *image, const float *value, int width, int height) {
    ms_image(image, value, 0, width, height, 1);
    image->dim[0].stride = 0;
}

void ms_image_rgba(MsImage *image, const void *pixels, ptrdiff_t row, int half, int width,
                   int height) {
    if (!half) {
        ms_image(image, pixels, row, width, height, 4);
        return;
    }
    // Float pixels of its own (GPU memory, or on the CPU host memory ms_run gives it).
    ms_image(image, NULL, (ptrdiff_t)width * 4, width, height, 4);
    image->half = pixels;
    image->half_row = row;
}

void ms_image_half(MsImage *image, const void *pixels, ptrdiff_t row, int width, int height) {
    // Two 32-bit integers per pixel (src/halide/convert_generator.cpp): rows of RGBA halves
    // are a whole number of them.
    ms_image(image, NULL, row / 2, width, height, 2);
    image->b.host = (uint8_t *)pixels;
    image->b.type.code = halide_type_uint;
}
