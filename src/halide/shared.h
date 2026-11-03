// What the generators share: the layout of the images, and the compensated running sums of
// the GPU versions that sum whole lines (blur.h's prefix boxes, dt_generator.cpp's RowSums).

#pragma once

#include "Halide.h"

namespace ms {

using namespace Halide;

// Interleaved RGBA: channels next to each other.
template <typename B> void interleaved(B &&b) {
    b.dim(0).set_stride(4);
    b.dim(2).set_stride(1).set_bounds(0, 4);
}

// Running sums over a line in float (Metal has no double), chunk by chunk: the sum of the
// chunks before each chunk is compensated (Kahan: the sum, and what its rounding left out),
// strict (kept as is). The next (sum, left out) from the previous ones and the next chunk.
inline Tuple kahan_add(Expr sum, Expr left_out, Expr chunk) {
    Expr add = chunk - left_out;
    Expr total = strict_float(sum + add);
    return Tuple(total, strict_float((total - sum) - add));
}

// A running sum at a sample: the sum of the chunks before its chunk, what its rounding left
// out, and its chunk up to it.
struct Compensated {
    Expr chunks, left_out, inside;
};

// hi - lo: the large sums of chunks first, so that the rounding of their difference is about
// the difference's size, not theirs.
inline Expr difference(const Compensated &hi, const Compensated &lo) {
    return strict_float(((hi.chunks - lo.chunks) - (hi.left_out - lo.left_out)) +
                        (hi.inside - lo.inside));
}

} // namespace ms
