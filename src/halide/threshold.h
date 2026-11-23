// The threshold of the effects that light the bright parts of the image (Glow, Glint): per
// channel T = max(0, (s - t - add) / (1 - t)) (t >= 1: s where s - add > t, else 0); the
// pixel is s x (T_r + T_g + T_b) / (s_r + s_g + s_b), at most s (0 where that sum is not
// above 0). `keep_at_zero` != 0: a threshold of 0 or less with nothing added leaves s as it
// is, negatives included.

#pragma once

#include "Halide.h"

namespace ms {

using namespace Halide;

struct Threshold {
    Expr threshold, add_r, add_g, add_b, keep_at_zero;
};

// `s` (x, y, c), RGB: its threshold, RGB (c = 3: 0).
inline Func threshold_of(Func s, const Threshold &p) {
    Var x = s.args()[0], y = s.args()[1], c = s.args()[2];
    Func t("t"), out("thresholded");
    Expr add = select(c == 0, p.add_r, c == 1, p.add_g, p.add_b);
    Expr vc = s(x, y, c) - add;
    // (A threshold of 1 or more: the division unused, its divisor 1 rather than 0 or less:
    // inf and NaN there made CUDA twice as slow.)
    Expr divisor = select(p.threshold < 1.0f, 1.0f - p.threshold, 1.0f);
    t(x, y, c) = select(p.threshold < 1.0f, max(0.0f, (vc - p.threshold) / divisor),
                        select(vc > p.threshold, s(x, y, c), 0.0f));
    Expr total_t = t(x, y, 0) + t(x, y, 1) + t(x, y, 2);
    Expr total_s = s(x, y, 0) + s(x, y, 1) + s(x, y, 2);
    Expr kept = p.keep_at_zero != 0 && p.threshold <= 0.0f && p.add_r == 0.0f && p.add_g == 0.0f &&
                p.add_b == 0.0f;
    out(x, y, c) = select(c >= 3, 0.0f, kept, s(x, y, c), total_s > 0.0f,
                          min(s(x, y, c) * total_t / total_s, s(x, y, c)), 0.0f);
    return out;
}

} // namespace ms
