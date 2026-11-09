// MS_Glow in Halide, two kernels (effects/ms_glow.c runs them in order):
//   ms_glow_blur     the bright parts of the Source (the threshold below), blurred: 3 boxes
//                    along x then 3 along y (blur.h), a total radius per channel and axis
//   ms_glow_combine  the glow scaled by brightness x colour, combined with the Source
//
// The threshold of a pixel s (RGB): s mixed towards its alpha by Glow From Alpha, times the
// Mask's RGB; per channel T = max(0, (s - t - add) / (1 - t)) (t >= 1: s where s - add > t,
// else 0); the pixel is s x (T_r + T_g + T_b) / (s_r + s_g + s_b), at most s.
//
// The blur: blur.h's blur3_subpixel.
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "blur.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

struct Threshold {
    Expr threshold, add_r, add_g, add_b, from_alpha, has_mask, invert_mask;
};

// The threshold of the Source (see above), RGB (c = 3: 0).
Func thresholded(Func source, Func mask, const Threshold &p) {
    Func s("s"), v("v"), t("t"), out("thresholded");
    Expr m = select(p.invert_mask != 0, 1.0f - mask(x, y, c), mask(x, y, c));
    s(x, y, c) = (source(x, y, c) + (source(x, y, 3) - source(x, y, c)) * p.from_alpha) *
                 select(p.has_mask != 0, m, 1.0f);
    Expr add = select(c == 0, p.add_r, c == 1, p.add_g, p.add_b);
    Expr vc = s(x, y, c) - add;
    // (A threshold of 1 or more: the division unused, its divisor 1 rather than 0 or less:
    // inf and NaN there made CUDA twice as slow.)
    Expr divisor = select(p.threshold < 1.0f, 1.0f - p.threshold, 1.0f);
    t(x, y, c) = select(p.threshold < 1.0f, max(0.0f, (vc - p.threshold) / divisor),
                        select(vc > p.threshold, s(x, y, c), 0.0f));
    Expr total_t = t(x, y, 0) + t(x, y, 1) + t(x, y, 2);
    Expr total_s = s(x, y, 0) + s(x, y, 1) + s(x, y, 2);
    out(x, y, c) =
        select(c < 3 && total_s > 0.0f, min(s(x, y, c) * total_t / total_s, s(x, y, c)), 0.0f);
    return out;
}

} // namespace

class GlowBlur : public Generator<GlowBlur> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 3>> mask{"mask"}; // the Source when there is no Mask
    Input<int> has_mask{"has_mask"}, invert_mask{"invert_mask"};
    Input<float> threshold{"threshold"}, add_r{"add_r"}, add_g{"add_g"}, add_b{"add_b"};
    Input<float> from_alpha{"from_alpha"};
    // Total radius per channel and axis: whole pixels and the fraction past them.
    Input<int> rx_r{"rx_r"}, rx_g{"rx_g"}, rx_b{"rx_b"}, ry_r{"ry_r"}, ry_g{"ry_g"}, ry_b{"ry_b"};
    Input<float> fx_r{"fx_r"}, fx_g{"fx_g"}, fx_b{"fx_b"}, fy_r{"fy_r"}, fy_g{"fy_g"}, fy_b{"fy_b"};
    Input<int> transparent{"transparent"}; // Edge Mode: 0 outside the image (else mirrored)
    Output<Buffer<float, 3>> glow{"glow"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func src("src"), msk("msk");
        src(x, y, c) = source(x, y, c);
        msk(x, y, c) = mask(x, y, c);
        Func th = thresholded(src, msk,
                              {threshold, add_r, add_g, add_b, from_alpha, has_mask, invert_mask});
        std::vector<ms_blur::Box> boxes;
        Func f = ms_blur::blur3_subpixel(
            th, select(c == 0, rx_r, c == 1, rx_g, rx_b), select(c == 0, fx_r, c == 1, fx_g, fx_b),
            max(rx_r, max(rx_g, rx_b)), select(c == 0, ry_r, c == 1, ry_g, ry_b),
            select(c == 0, fy_r, c == 1, fy_g, fy_b), max(ry_r, max(ry_g, ry_b)), transparent,
            source.width(), source.height(), gpu, "glow", boxes);
        glow(x, y, c) = f(x, y, c);

        ms::interleaved(source);
        ms::interleaved(mask);
        ms::interleaved(glow);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(boxes, gpu, 4); // RGB and a zero alpha: whole float4 on the GPU
        // The threshold once per pixel (the first boxes read it many times).
        th.compute_root().bound(c, 0, 4).reorder_storage(c, x, y).reorder(c, x, y);
        if (gpu)
            th.vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            th.unroll(c).vectorize(x, 8).parallel(y);
        if (gpu)
            glow.bound(c, 0, 4).reorder(c, x, y).vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16,
                                                                       16);
        else
            glow.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
    }
};

class GlowCombine : public Generator<GlowCombine> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 3>> glow{"glow"}; // ms_glow_blur
    Input<Buffer<float, 3>> mask{"mask"}; // the Source when there is no Mask
    Input<int> has_mask{"has_mask"}, invert_mask{"invert_mask"};
    Input<float> threshold{"threshold"}, add_r{"add_r"}, add_g{"add_g"}, add_b{"add_b"};
    Input<float> from_alpha{"from_alpha"};
    Input<float> gain_r{"gain_r"}, gain_g{"gain_g"}, gain_b{"gain_b"}; // Brightness x Color
    Input<int> combine{"combine"}; // Mult, Add, Screen, Difference, Overlay
    Input<float> source_opacity{"source_opacity"}, under{"under"}, affect_alpha{"affect_alpha"};
    Input<int> show_threshold{"show_threshold"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func src("src"), msk("msk");
        src(x, y, c) = source(x, y, c);
        msk(x, y, c) = mask(x, y, c);
        Func th = thresholded(src, msk,
                              {threshold, add_r, add_g, add_b, from_alpha, has_mask, invert_mask});

        // The glow, and the Source scaled by Source Opacity.
        Func g("g");
        g(x, y, c) = glow(x, y, c) * select(c == 0, gain_r, c == 1, gain_g, gain_b);
        Expr a = source(x, y, 3) * source_opacity;
        Expr s = source(x, y, c) * source_opacity;
        Expr gc = g(x, y, c);
        // The Combine mode; RGB with Screen and Overlay above 1 (either side): the larger.
        auto combined = [&](Expr u, Expr gl, bool rgb) {
            Expr high = rgb ? (u > 1.0f || gl > 1.0f) : Expr(false);
            return select(
                combine == 0, u * gl, combine == 1, u + gl, combine == 2,
                select(high, max(u, gl), u + gl - u * gl), combine == 3, abs(u - gl),
                select(high, max(u, gl),
                       select(u < 0.5f, 2.0f * u * gl, 1.0f - 2.0f * (1.0f - u) * (1.0f - gl))));
        };
        Expr mixed = combined(s, gc, true);
        // Glow Under Source: towards the Source over the glow.
        Expr over = s + gc * (1.0f - a);
        Expr rgb = mixed + (over - mixed) * under;
        // The alpha: combined the same way with the largest of the glow's channels times
        // Affect Alpha.
        Expr ga = max(g(x, y, 0), max(g(x, y, 1), g(x, y, 2))) * affect_alpha;
        Expr alpha = combined(a, ga, false);
        Expr result = select(c == 3, alpha, rgb);
        output(x, y, c) = select(show_threshold != 0, select(c == 3, 1.0f, th(x, y, c)), result);

        ms::interleaved(source);
        ms::interleaved(glow);
        ms::interleaved(mask);
        ms::interleaved(output);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        if (gpu)
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
    }
};

HALIDE_REGISTER_GENERATOR(GlowBlur, ms_glow_blur)
HALIDE_REGISTER_GENERATOR(GlowCombine, ms_glow_combine)
