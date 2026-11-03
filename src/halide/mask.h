// The mask value of the effects, from their Mask input (interleaved RGBA): its luma
// (5 R + 8 G + 3 B) / 16, or its alpha, maybe inverted.

#pragma once

#include "Halide.h"

namespace ms_mask {

using namespace Halide;

inline Expr luma(Func rgba, Expr x, Expr y) {
    return (5.0f * rgba(x, y, 0) + 8.0f * rgba(x, y, 1) + 3.0f * rgba(x, y, 2)) * 0.0625f;
}

// `alpha`, `invert`: the Mask Use (0 luma, 1 alpha) and Invert Mask parameters.
inline Expr value(Func rgba, Expr x, Expr y, Expr alpha, Expr invert) {
    Expr v = select(alpha != 0, rgba(x, y, 3), luma(rgba, x, y));
    return select(invert != 0, 1.0f - v, v);
}

} // namespace ms_mask
