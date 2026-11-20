// MS_Glow in Halide, two kernels (effects/ms_glow.c runs them in order):
//   ms_glow_blur     the bright parts of the Source (the threshold below), blurred: 3 boxes
//                    along x then 3 along y (blur.h), a total radius per channel and axis
//   ms_glow_combine  the glow scaled by brightness x colour and the Atmosphere, combined with
//                    the Source
//
// The threshold of a pixel s (RGB): s mixed towards its alpha by Glow From Alpha, times the
// Mask's RGB; then threshold.h's.
//
// The blur: blur.h's blur3_subpixel.
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "blur.h"
#include "noise.h"
#include "shared.h"
#include "threshold.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

struct Threshold {
    Expr threshold, add_r, add_g, add_b, from_alpha, has_mask, invert_mask;
};

// The threshold of the Source (see above), RGB (c = 3: 0).
Func thresholded(Func source, Func mask, const Threshold &p) {
    Func s("s");
    Expr m = select(p.invert_mask != 0, 1.0f - mask(x, y, c), mask(x, y, c));
    s(x, y, c) = (source(x, y, c) + (source(x, y, 3) - source(x, y, c)) * p.from_alpha) *
                 select(p.has_mask != 0, m, 1.0f);
    return ms::threshold_of(s, {p.threshold, p.add_r, p.add_g, p.add_b, Expr(0)});
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
    Input<float> light{"light"}; // Light Background
    Input<int> show_threshold{"show_threshold"};
    // The Atmosphere (effects/atmosphere.h): x 1 + amp x a noise (fbm of `octaves`, frequency
    // `freq` per pixel, `persistence`, at time `z`); none with 0 octaves
    Input<float> amp{"amp"}, freq{"freq"}, persistence{"persistence"}, z{"z"};
    Input<int> octaves{"octaves"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func atmosphere("atmosphere");
        Expr n = ms_noise::fbm((cast<float>(x) + 0.5f) * freq, (cast<float>(y) + 0.5f) * freq, z,
                               octaves, persistence, 12, "octave");
        atmosphere(x, y) = select(octaves > 0, 1.0f + amp * n, 1.0f);
        Func src("src"), msk("msk");
        src(x, y, c) = source(x, y, c);
        msk(x, y, c) = mask(x, y, c);
        Func th = thresholded(src, msk,
                              {threshold, add_r, add_g, add_b, from_alpha, has_mask, invert_mask});

        // The glow, and the Source scaled by Source Opacity.
        Func g("g");
        g(x, y, c) = glow(x, y, c) * select(c == 0, gain_r, c == 1, gain_g, gain_b) * atmosphere(x, y);
        Expr a = source(x, y, 3) * source_opacity;
        Expr s = source(x, y, c) * source_opacity;
        Expr gc = g(x, y, c);
        // The Combine mode C(u, gl) = c0 + c1 gl, u the Source (x Source Opacity), gl the glow;
        // Light Background L (m = 1 - L) with v the Source as it is, va the alpha (x Source
        // Opacity), gp = gl (m + 2 L v):
        //   m^2 C + (1 - m^2) u + 2 L m gl v + L m^2 gl (1 - 2 v)(1 - c1) + (1 - va) L k gp,
        //   k 1 with Add, Screen and Difference, else L; L = 0: the plain mode.
        // The branches compare u with gp: Difference's sign; Screen and Overlay (from u = 0.5:
        // 2 u gl below) the larger of u and gl when either u or gp is above 1 (RGB; the
        // alpha: Overlay with gp above 1).
        Expr L = light, m = 1.0f - light;
        auto combined = [&](Expr u, Expr gl, Expr v, Expr va, bool rgb, Expr combine) {
            Expr gp = gl * (m + 2.0f * L * v);
            Expr high = rgb ? (u > 1.0f || gp > 1.0f) : (combine == 4 && gp > 1.0f);
            Expr over = u > gp;
            Expr c0 = select(combine == 0, 0.0f, combine == 1, u, combine == 2,
                             select(high, select(over, u, 0.0f), u), combine == 3,
                             select(u >= gp, u, -u),
                             select(u < 0.5f, 0.0f, high, select(over, u, 0.0f), 2.0f * u - 1.0f));
            Expr c1 = select(combine == 0, u, combine == 1, 1.0f, combine == 2,
                             select(high, select(over, 0.0f, 1.0f), 1.0f - u), combine == 3,
                             select(u >= gp, -1.0f, 1.0f),
                             select(u < 0.5f, 2.0f * u, high, select(over, 0.0f, 1.0f),
                                    2.0f * (1.0f - u)));
            Expr k = select(combine == 1 || combine == 2 || combine == 3, 1.0f, L);
            return m * m * (c0 + c1 * gl) + (1.0f - m * m) * u + 2.0f * L * m * gl * v +
                   L * m * m * gl * (1.0f - 2.0f * v) * (1.0f - c1) + (1.0f - va) * L * k * gp;
        };
        Expr mixed = combined(s, gc, source(x, y, c), a, true, this->combine);
        // Glow Under Source: towards the Source over the glow (as Light Background turns it,
        // times k).
        Expr k = select(combine == 1 || combine == 2 || combine == 3, 1.0f, L);
        auto over = [&](Expr u, Expr gl, Expr v) {
            return u + k * gl * (m + 2.0f * L * v) * (1.0f - a);
        };
        Expr rgb = mixed + (over(s, gc, source(x, y, c)) - mixed) * under;
        // The alpha: the same with the largest of the glow's channels times Affect Alpha
        // (Difference as Screen), Light Background from the largest of the Source's; at most 1.
        Expr ga = max(g(x, y, 0), max(g(x, y, 1), g(x, y, 2))) * affect_alpha;
        Expr vmax = max(source(x, y, 0), max(source(x, y, 1), source(x, y, 2)));
        Expr mixed_a = combined(a, ga, vmax, a, false, select(this->combine == 3, 2, this->combine));
        Expr alpha = min(mixed_a + (over(a, ga, vmax) - mixed_a) * under, 1.0f);
        Expr result = select(c == 3, alpha, rgb);
        output(x, y, c) =
            select(show_threshold != 0, select(c == 3, 1.0f, th(x, y, c) * atmosphere(x, y)), result);

        ms::interleaved(source);
        ms::interleaved(glow);
        ms::interleaved(mask);
        ms::interleaved(output);
        ms::schedule_rgba(output, gpu);
        // the Atmosphere once per pixel (not per channel)
        if (gpu)
            atmosphere.compute_at(output, Var("xi"));
        else
            atmosphere.compute_at(output, x).vectorize(x, 8);
    }
};

HALIDE_REGISTER_GENERATOR(GlowBlur, ms_glow_blur)
HALIDE_REGISTER_GENERATOR(GlowCombine, ms_glow_combine)
