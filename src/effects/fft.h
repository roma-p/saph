// Fast Fourier transforms of images for the effects that convolve by FFT, with the stages of
// src/halide/fft_generator.cpp: images of 4 floats per pixel, two complex numbers.

#pragma once

#include "engine/engine.h"

// The smallest transform length of at least `length`: a product of 2, 3 and 5.
int ms_fft_length(int length);

// Transforms `*data` along x then y (`inverse` 0), or back along y then x, not scaled (the
// inverse gives the image times width x height). The stages go back and forth between
// `*data` and `*scratch`, of the same size; on return `*data` is the one holding the result
// and `*scratch` the other. Returns Halide's status (0: success).
int ms_fft(MsApi api, MsImage **data, MsImage **scratch, int inverse);
