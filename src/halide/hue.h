// Hue Shift of the glare effects (Glint, Glare): the HSV hue turned by `hue` (in turns),
// saturation and value kept, the colour then scaled back to its luma (5 R + 8 G + 3 B) / 16.

#pragma once

#include "Halide.h"

namespace ms {

using namespace Halide;

// Channel c (0..2) of (r, g, b) with its hue turned; c >= 3, grey or hue 0: `other`.
inline Expr hue_shifted(Expr r, Expr g, Expr b, Expr c, Expr hue, Expr other) {
    Expr mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
    Expr safe = select(d > 0.0f, d, 1.0f);
    // The hue in sixths: 0 red, 2 green, 4 blue.
    Expr h6 =
        select(mx == r, (g - b) / safe, mx == g, (b - r) / safe + 2.0f, (r - g) / safe + 4.0f);
    h6 = h6 + 6.0f * hue;
    h6 = h6 - 6.0f * floor(h6 / 6.0f);
    auto part = [&](float n) {
        Expr k = n + h6;
        k = k - 6.0f * floor(k / 6.0f);
        return mx - d * clamp(min(k, 4.0f - k), 0.0f, 1.0f);
    };
    Expr nr = part(5.0f), ng = part(3.0f), nb = part(1.0f);
    Expr l0 = 0.3125f * r + 0.5f * g + 0.1875f * b;
    Expr l1 = 0.3125f * nr + 0.5f * ng + 0.1875f * nb;
    Expr scale = select(l1 > 0.0f, l0 / l1, 0.0f);
    Expr shifted = select(c == 0, nr, c == 1, ng, nb) * scale;
    // (grey within float rounding: as grey, the CPU and the GPU would pick different hues)
    return select(c >= 3 || d <= 1e-4f * abs(mx) || hue == 0.0f, other, shifted);
}

} // namespace ms
