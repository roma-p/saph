// MS_Glint in Halide, three kernels (effects/ms_glint.c runs them in order):
//   ms_glint_source   the glints' source: the Source towards its alpha by Glint From Alpha,
//                     times the Mask, blurred by Threshold Blur (blur.h's blur3_subpixel,
//                     mirrored), then threshold.h's threshold (negatives kept at 0)
//   ms_glint_ray      the rays of one direction (x, y, or a diagonal): per channel a 3-box
//                     blur of the source along the direction, times its amplitude, in [0, 1];
//                     `total` (1 for the first direction) times 1 minus that: the rays of
//                     all directions screened together
//   ms_glint_combine  the rays (1 - total): Hue Shift, Blur Glint (blurred=true; else none),
//                     then screened over the Source scaled by Source Opacity (or under it),
//                     Affect Alpha
//
// The rays along x and y have nothing beyond the image. A diagonal runs along a buffer n =
// min(width, height) pixels long that starts at its lower-left end in the image (up to the
// right, x - y constant) or ends at its lower-right end (down to the right, x + y
// constant); the boxes keep their sums along that buffer, nothing beyond it: some
// diagonals reach past the image on one side. Each box at most as wide as its line (the
// image's width, height, or n).
//
// Hue Shift: the HSV hue turned (saturation and value kept), the colour then scaled back to
// its luma (5 R + 8 G + 3 B) / 16. The screen: max(b + r (1 - b), r) with b at least 0 and
// 1 - b at least 0.
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "blur.h"
#include "shared.h"
#include "threshold.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

} // namespace

class GlintSource : public Generator<GlintSource> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 3>> mask{"mask"}; // the Source when there is no Mask
    Input<int> has_mask{"has_mask"}, invert_mask{"invert_mask"};
    Input<int> mask_type{"mask_type"}; // Luma, Color, Alpha (the Mask's first channel)
    Input<float> from_alpha{"from_alpha"};
    Input<int> blur{"blur"};          // Threshold Blur: whole pixels
    Input<float> blur_frac{"blur_frac"}; // and the fraction past them
    Input<float> threshold{"threshold"}, add_r{"add_r"}, add_g{"add_g"}, add_b{"add_b"};
    Output<Buffer<float, 3>> glints{"glints"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func s("s");
        Expr m = select(mask_type == 0, 0.3125f * mask(x, y, 0) + 0.5f * mask(x, y, 1) +
                                            0.1875f * mask(x, y, 2),
                        mask_type == 1, mask(x, y, min(c, 2)), mask(x, y, 0));
        m = select(invert_mask != 0, 1.0f - m, m);
        s(x, y, c) = (source(x, y, c) + (source(x, y, 3) - source(x, y, c)) * from_alpha) *
                     select(has_mask != 0, m, 1.0f);
        std::vector<ms_blur::Box> boxes;
        Func b = ms_blur::blur3_subpixel(s, blur, blur_frac, blur, blur, blur_frac, blur, Expr(0),
                                         source.width(), source.height(), gpu, "tblur", boxes);
        Func th = ms::threshold_of(b, {threshold, add_r, add_g, add_b, Expr(1)});
        glints(x, y, c) = th(x, y, c);

        ms::interleaved(source);
        ms::interleaved(mask);
        ms::interleaved(glints);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(boxes, gpu, 4);
        s.compute_root().bound(c, 0, 4).reorder_storage(c, x, y).reorder(c, x, y);
        if (gpu) {
            s.vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            glints.bound(c, 0, 4).reorder(c, x, y).vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16,
                                                                         16);
        } else {
            s.unroll(c).vectorize(x, 8).parallel(y);
            glints.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
        }
    }
};

class GlintRay : public Generator<GlintRay> {
  public:
    GeneratorParam<int> direction{"direction", 0}; // 0 x, 1 y, 2 up to the right, 3 down to it
    Input<Buffer<float, 3>> glints{"glints"};      // ms_glint_source
    Input<Buffer<float, 3>> total{"total"};        // the rays so far (unused when first)
    Input<int> first{"first"};
    Input<int> r_r{"r_r"}, r_g{"r_g"}, r_b{"r_b"}; // total radius per channel
    Input<float> amp_r{"amp_r"}, amp_g{"amp_g"}, amp_b{"amp_b"};
    Output<Buffer<float, 3>> out{"out"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr w = glints.width(), h = glints.height();
        Expr r = select(c == 0, r_r, c == 1, r_g, r_b);
        Expr r_max = max(r_r, max(r_g, r_b));
        // 3 boxes along x (or y) of `line`, 0 beyond [0, length) x [0, lines), each box at
        // most as wide as the line (radius (length - 1) / 2).
        std::vector<ms_blur::Box> boxes;
        auto blur = [&](Func line, bool along_x, Expr length, Expr lines) {
            Func f = line;
            Expr limit = (length - 1) / 2;
            for (int i = 0; i < 3; ++i) {
                boxes.push_back(ms_blur::box(f, min(ms_blur::box_radius(r, i), limit), along_x,
                                             along_x ? length : lines, along_x ? lines : length,
                                             gpu, ms_blur::Boxes::prefix, "ray" + std::to_string(i),
                                             {min(r_max / 3 + 1, limit), Expr(), Expr(1)}));
                f = boxes.back().out;
            }
            return f;
        };
        Func in("in");
        in(x, y, c) = glints(x, y, c);
        Func line("line"); // the diagonals: line i, position t along it (x), 0 to n - 1
        Var t = x, i = y;
        Expr n = min(w, h);
        Expr ray;
        if (direction == 0) {
            ray = blur(in, true, w, h)(x, y, c);
        } else if (direction == 1) {
            // The columns as rows (boxes along x are faster): line x of the image, position y.
            line(t, i, c) = glints(clamp(i, 0, w - 1), clamp(t, 0, h - 1), c);
            ray = blur(line, true, h, w)(y, x, c);
        } else if (direction == 2) {
            // Up to the right: x - y = k, line i = k + h - 1, from x = max(0, k).
            Expr k = i - (h - 1);
            Expr lx = max(0, k) + t, ly = lx - k;
            line(t, i, c) = select(lx < w && ly >= 0 && ly < h,
                                   glints(clamp(lx, 0, w - 1), clamp(ly, 0, h - 1), c), 0.0f);
            // (Read clamped: Halide's bounds of the gather would cover far more than the lines.)
            ray = blur(line, true, n, w + h - 1)(clamp(x - max(0, x - y), 0, n - 1),
                                                 clamp(x - y + h - 1, 0, w + h - 2), c);
        } else {
            // Down to the right: x + y = i, back from x = min(w - 1, i).
            Expr lx = min(w - 1, i) - t, ly = i - lx;
            line(t, i, c) = select(lx >= 0 && ly >= 0 && ly < h,
                                   glints(clamp(lx, 0, w - 1), clamp(ly, 0, h - 1), c), 0.0f);
            ray = blur(line, true, n, w + h - 1)(clamp(min(w - 1, x + y) - x, 0, n - 1),
                                                 clamp(x + y, 0, w + h - 2), c);
        }
        Expr amp = select(c == 0, amp_r, c == 1, amp_g, amp_b);
        Expr kept = 1.0f - clamp(ray * amp, 0.0f, 1.0f);
        out(x, y, c) = select(c >= 3, 0.0f, select(first != 0, 1.0f, total(x, y, c)) * kept);

        ms::interleaved(glints);
        ms::interleaved(total);
        ms::interleaved(out);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(boxes, gpu, 4);
        if (direction >= 1) {
            line.compute_root().bound(c, 0, 4).reorder_storage(c, t, i).reorder(c, t, i);
            if (gpu)
                line.vectorize(c).gpu_tile(t, i, xo, yo, xi, yi, 16, 16);
            else
                line.unroll(c).vectorize(t, 8).parallel(i);
        }
        if (gpu)
            out.bound(c, 0, 4).reorder(c, x, y).vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            out.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
    }
};

class GlintCombine : public Generator<GlintCombine> {
  public:
    GeneratorParam<bool> blurred{"blurred", false}; // Blur Glint (else none)
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 3>> total{"total"}; // ms_glint_ray's (unused without rays)
    Input<int> has_rays{"has_rays"};
    Input<float> hue{"hue"};       // Hue Shift, in turns
    Input<int> blur{"blur"};       // Blur Glint: whole pixels
    Input<float> blur_frac{"blur_frac"};
    Input<float> source_opacity{"source_opacity"}, under{"under"}, affect_alpha{"affect_alpha"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        // The rays, their hue turned.
        Func rays("rays"), turned("turned");
        rays(x, y, c) = select(has_rays != 0 && c < 3, 1.0f - total(x, y, c), 0.0f);
        Expr r = rays(x, y, 0), g = rays(x, y, 1), b = rays(x, y, 2);
        Expr mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
        Expr safe = select(d > 0.0f, d, 1.0f);
        // The hue in sixths: 0 red, 2 green, 4 blue.
        Expr h6 = select(mx == r, (g - b) / safe, mx == g, (b - r) / safe + 2.0f,
                         (r - g) / safe + 4.0f);
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
        turned(x, y, c) = select(c >= 3 || d <= 0.0f || hue == 0.0f, rays(x, y, c), shifted);
        std::vector<ms_blur::Box> boxes;
        Func ray("ray");
        if (blurred)
            ray(x, y, c) = ms_blur::blur3_subpixel(turned, blur, blur_frac, blur, blur, blur_frac,
                                                   blur, Expr(0), source.width(), source.height(),
                                                   gpu, "gblur", boxes)(x, y, c);
        else
            ray(x, y, c) = turned(x, y, c);

        // Screened over the Source scaled by Source Opacity, or under it.
        Expr a = source(x, y, 3) * source_opacity;
        Expr s = source(x, y, c) * source_opacity;
        Expr rc = ray(x, y, c);
        Expr s0 = max(s, 0.0f);
        Expr on = max(s0 + rc * max(1.0f - s0, 0.0f), rc);
        Expr below = s + (1.0f - a) * rc;
        Expr rgb = on + (below - on) * under;
        Expr glint_alpha = max(ray(x, y, 0), max(ray(x, y, 1), ray(x, y, 2))) * affect_alpha;
        Expr alpha = a + (1.0f - a) * glint_alpha;
        output(x, y, c) = select(c == 3, alpha, rgb);

        ms::interleaved(source);
        ms::interleaved(total);
        ms::interleaved(output);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(boxes, gpu, 4);
        for (Func f : {turned, ray}) {
            if (!blurred && f.name() == ray.name())
                continue; // inline
            f.compute_root().bound(c, 0, 4).reorder_storage(c, x, y).reorder(c, x, y);
            if (gpu)
                f.vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            else
                f.unroll(c).vectorize(x, 8).parallel(y);
        }
        if (gpu)
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
    }
};

HALIDE_REGISTER_GENERATOR(GlintSource, ms_glint_source)
HALIDE_REGISTER_GENERATOR(GlintRay, ms_glint_ray)
HALIDE_REGISTER_GENERATOR(GlintCombine, ms_glint_combine)
