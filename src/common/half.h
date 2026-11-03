// 16-bit floats ("half", IEEE 754 binary16) as hosts store them, to and from float. The
// images themselves are converted by the kernels (src/halide/convert_generator.cpp); these
// are for single values and tests.

#pragma once

#include <stdint.h>

// Rounds to the nearest half (ties to even); infinities and NaN kept.
uint16_t ms_half_from_float(float f);
float ms_float_from_half(uint16_t h);
