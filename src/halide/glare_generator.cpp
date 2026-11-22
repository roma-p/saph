// MS_Glare in Halide (effects/ms_glare.c runs the kernels; the glints' source, thresholded,
// is Glint's ms_glint_source; the transforms are effects/fft.c's, the product RackDefocus's):
//   ms_glare_kernel       the glare of one spot of strength 1 at pixel (0, 0) of the
//                         transform (wrapped around): the sum of the style's elements
//                         (glare_layout.h), each a radial gradient (colour stops, sectors,
//                         polygons, waves) or ray lines
//   ms_glare_label_init   the spots: each pixel of the thresholded image above 0 labelled
//                         with its own index (y x width + x), the others none() (the largest)
//   ms_glare_label_round  one round of the connected regions (8 neighbours): the smallest
//                         label around, then followed twice (label of the pixel the label
//                         names); after enough rounds each region holds its first pixel's
//                         index
//   ms_glare_spots        per region: its sums (colour, weight, weighted offsets), then one
//                         impulse at its centroid (bilinear), of strength min(m b, cap) /
//                         norm with m its largest channel, its colour kept, times Brightness
//                         x Scale Colors again (b), into the transform's image
//   ms_glare_half         Glare Res Half: an image at half its size (means of 2 x 2 pixels)
//   ms_glare_finish       the glare (the inverse transform, scaled; at half size, enlarged
//                         bilinearly): Hue Shift, Saturation,
//                         Blur Glare, then screened over the Source x Source Opacity, Glare
//                         Under Source, Affect Alpha (as Glint)
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first (y up); the labels
// one unsigned 32-bit integer per pixel.

#include "Halide.h"
#include "blur.h"
#include "glare_elements.h"
#include "glare_layout.h"
#include "hue.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c"), e("e");

// The label of a pixel in no spot (the largest).
Expr none() { return Internal::UIntImm::make(UInt(32), 0xffffffffu); }

} // namespace

class GlareKernel : public Generator<GlareKernel> {
  public:
    Input<Buffer<float, 2>> rows{"rows"}; // (field, element), glare_layout.h
    Input<int> count{"count"};
    Output<Buffer<float, 3>> kernel{"kernel"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr nx = kernel.width(), ny = kernel.height();
        // The offset from the centre, wrapped around (pixel (0, 0) is the centre).
        Expr dxi = select(x < (nx + 1) / 2, x, x - nx), dyi = select(y < (ny + 1) / 2, y, y - ny);
        Expr dx = cast<float>(dxi), dy = cast<float>(dyi);
        ms_glare::Field field = [&](Expr i, Expr el) {
            return rows(clamp(i, 0, GLARE_FIELDS - 1), clamp(el, 0, GLARE_MAX_ELEMENTS - 1));
        };
        // The noises around the noisy polar flares (glare_elements.h), each its own slot.
        ms_glare::PolarNoises pn;
        ms_glare::polar_noises(pn, field, [](Expr el) { return el; });
        Func element("element");
        element(x, y, c, e) = ms_glare::element_value(field, dx, dy, c, e, &pn.angular);
        // The sum of the elements, each where it reaches.
        Func sum_of("sum_of");
        RDom el(0, count, "el");
        Expr reach = ms_glare::element_reach(field, el);
        el.where(cast<float>(dxi * dxi + dyi * dyi) <= reach * reach);
        sum_of(x, y, c) = 0.0f;
        sum_of(x, y, c) += element(x, y, c, el);
        kernel(x, y, c) = select(c < 3, sum_of(x, y, min(c, 2)), 0.0f);

        rows.dim(0).set_bounds(0, GLARE_FIELDS);
        rows.dim(1).set_bounds(0, GLARE_MAX_ELEMENTS);
        ms::interleaved(kernel);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        kernel.bound(c, 0, 4).reorder(c, x, y);
        sum_of.compute_at(kernel, gpu ? xi : x).bound(c, 0, 3).reorder(c, x, y);
        sum_of.update().reorder(c, el);
        if (gpu)
            kernel.unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            kernel.unroll(c).parallel(y);
        // (the noises of every element: none but the noisy ones cost anything, their octaves
        // and taps empty)
        const int ANGLES = ms_glare::NOISE_ANGLES;
        pn.noises.compute_root().bound(pn.i, 0, ANGLES).bound(pn.e, 0, GLARE_MAX_ELEMENTS)
            .bound(pn.which, 0, 2);
        pn.thresholded.compute_root().bound(pn.i, 0, ANGLES).bound(pn.e, 0, GLARE_MAX_ELEMENTS);
        if (gpu) {
            Var io("io"), ii("ii");
            pn.noises.gpu_tile(pn.i, io, ii, 64);
            pn.thresholded.gpu_tile(pn.i, io, ii, 64);
        } else {
            pn.noises.parallel(pn.e).vectorize(pn.i, 8);
            pn.thresholded.parallel(pn.e).vectorize(pn.i, 8);
        }
    }
};

class GlareLabelInit : public Generator<GlareLabelInit> {
  public:
    Input<Buffer<float, 3>> glints{"glints"}; // the thresholded Source (ms_glint_source)
    Output<Buffer<uint32_t, 2>> labels{"labels"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr lit = glints(x, y, 0) + glints(x, y, 1) + glints(x, y, 2) > 0.0f;
        labels(x, y) = select(lit, cast<uint32_t>(y * glints.width() + x), none());
        ms::interleaved(glints);
        ms::schedule_2d(labels, gpu, 32, 8);
    }
};

class GlareLabelRound : public Generator<GlareLabelRound> {
  public:
    Input<Buffer<uint32_t, 2>> labels{"labels"};
    Output<Buffer<uint32_t, 2>> out{"out"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr w = labels.width(), h = labels.height();
        Func at = BoundaryConditions::constant_exterior(labels, none());
        Expr m = at(x, y);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx || dy)
                    m = min(m, at(x + dx, y + dy)); // none() is the largest
        // Followed twice: the label of the pixel it names.
        auto follow = [&](Expr l) {
            Expr i = cast<int>(min(l, cast<uint32_t>(w * h - 1)));
            return labels(clamp(i % w, 0, w - 1), clamp(i / w, 0, h - 1));
        };
        Expr j1 = min(m, follow(m));
        Expr j2 = min(j1, follow(j1));
        out(x, y) = select(labels(x, y) == none(), none(), j2);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        if (gpu)
            out.gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
        else
            out.parallel(y);
    }
};

class GlareSpots : public Generator<GlareSpots> {
  public:
    Input<Buffer<float, 3>> glints{"glints"};
    Input<Buffer<uint32_t, 2>> labels{"labels"};
    Input<float> cap{"cap"}, norm{"norm"};
    Input<float> bc_r{"bc_r"}, bc_g{"bc_g"}, bc_b{"bc_b"}; // Brightness x Scale Colors
    Input<int> pad_x{"pad_x"}, pad_y{"pad_y"};             // the image's place in `impulses`
    Output<Buffer<float, 3>> impulses{"impulses"};

    void generate() {
        // Halide has no atomic updates in Metal and Direct3D 12 code: there the sums and
        // impulses run on the CPU (Halide copies the images to and from the GPU around this
        // kernel).
        const Target t = get_target();
        const bool gpu = t.has_gpu_feature() && !t.has_feature(Target::Metal) &&
                         !t.has_feature(Target::D3D12Compute);
        Expr w = labels.width(), h = labels.height();
        // Each region's sums, at its first pixel: colour (0..2), weight (3), weighted offsets
        // from that pixel (4, 5: kept small for the floats). First summed along each row's
        // runs of one label (a scan, restarting where the label changes), then each run's
        // last pixel adds its run: few atomic additions to one region (one per row it spans,
        // not one per pixel: contended additions are slow on the GPU).
        Var k("k");
        Func values("values"), runs("runs"), sums("sums");
        Expr lv = labels(x, y);
        Expr lvi = cast<int>(min(lv, cast<uint32_t>(w * h - 1)));
        Expr weight = glints(x, y, 0) + glints(x, y, 1) + glints(x, y, 2);
        values(x, y, k) = select(lv == none(), 0.0f, k < 3, glints(x, y, clamp(k, 0, 2)), k == 3,
                                 weight, k == 4, weight * cast<float>(x - lvi % w),
                                 weight * cast<float>(y - lvi / w));
        RDom rx(1, max(w - 1, 1), "rx");
        runs(x, y, k) = values(x, y, k);
        Expr same = labels(rx, y) == labels(max(rx - 1, 0), y) && w > 1;
        runs(rx, y, k) = values(rx, y, k) + select(same, runs(rx - 1, y, k), 0.0f);
        sums(x, y, k) = 0.0f;
        RDom p(0, w, 0, h, 0, 6, "p");
        Expr label = labels(p.x, p.y);
        Expr run_end = p.x == w - 1 || labels(min(p.x + 1, w - 1), p.y) != label;
        p.where(label != none() && run_end);
        Expr li = cast<int>(min(label, cast<uint32_t>(w * h - 1)));
        Expr lx = li % w, ly = li / w;
        sums(clamp(lx, 0, w - 1), clamp(ly, 0, h - 1), p.z) += runs(p.x, p.y, p.z);

        // One impulse per region at its centroid: 4 corners (bilinear) x 3 channels.
        impulses(x, y, c) = 0.0f;
        RDom q(0, w, 0, h, 0, 4, 0, 3, "q");
        Expr mine = labels(q.x, q.y) == cast<uint32_t>(q.y * w + q.x);
        Expr total = sums(q.x, q.y, 3);
        q.where(mine && total > 0.0f);
        Expr cx = cast<float>(q.x) + sums(q.x, q.y, 4) / total;
        Expr cy = cast<float>(q.y) + sums(q.x, q.y, 5) / total;
        Expr fx = cx - floor(cx), fy = cy - floor(cy);
        Expr ix = cast<int>(floor(cx)) + q.z % 2, iy = cast<int>(floor(cy)) + q.z / 2;
        Expr corner = select(q.z % 2 == 1, fx, 1.0f - fx) * select(q.z / 2 == 1, fy, 1.0f - fy);
        auto bc = [&](Expr ch) { return select(ch == 0, bc_r, ch == 1, bc_g, bc_b); };
        Expr e0 = sums(q.x, q.y, 0) * bc_r, e1 = sums(q.x, q.y, 1) * bc_g, e2 = sums(q.x, q.y, 2) * bc_b;
        Expr m = max(e0, max(e1, e2));
        Expr ec = select(q.w == 0, e0, q.w == 1, e1, e2);
        Expr strength = select(m > 0.0f, min(m, cap) / norm * (ec / m) * bc(q.w), 0.0f);
        Expr nx = impulses.width(), ny = impulses.height();
        impulses(clamp(ix + pad_x, 0, nx - 1), clamp(iy + pad_y, 0, ny - 1), q.w) += corner * strength;

        ms::interleaved(glints);
        ms::interleaved(impulses);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        RVar po("po"), pi("pi"), qo("qo"), qi("qi");
        sums.compute_root().bound(k, 0, 6);
        runs.compute_root().bound(k, 0, 6).reorder(k, x, y);
        impulses.bound(c, 0, 4);
        if (gpu) {
            Var yo2("yo2"), yi2("yi2");
            runs.gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
            runs.update()
                .unroll(k)
                .split(y, yo2, yi2, 64, TailStrategy::GuardWithIf)
                .gpu_blocks(yo2)
                .gpu_threads(yi2);
            sums.gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
            sums.update().atomic().split(p.x, po, pi, 64).gpu_blocks(p.y).gpu_threads(pi);
            impulses.gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
            impulses.update().atomic().split(q.x, qo, qi, 64).gpu_blocks(q.y).gpu_threads(qi);
        } else {
            // The sums and impulses: one pass over the image each, in order.
            runs.parallel(y);
            runs.update().parallel(y);
            sums.parallel(y);
            sums.update().unscheduled();
            impulses.parallel(y);
            impulses.update().unscheduled();
        }
    }
};

class GlareHalf : public Generator<GlareHalf> {
  public:
    Input<Buffer<float, 3>> image{"image"};
    Output<Buffer<float, 3>> half{"half"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func at = BoundaryConditions::repeat_edge(image);
        half(x, y, c) = 0.25f * (at(2 * x, 2 * y, c) + at(2 * x + 1, 2 * y, c) +
                                 at(2 * x, 2 * y + 1, c) + at(2 * x + 1, 2 * y + 1, c));
        ms::interleaved(image);
        ms::interleaved(half);
        ms::schedule_float4(half, gpu, 32, 8);
    }
};

class GlareFinish : public Generator<GlareFinish> {
  public:
    Input<Buffer<float, 3>> data{"data"};     // the inverse transform, not scaled
    Input<Buffer<float, 3>> source{"source"};
    Input<int> pad_x{"pad_x"}, pad_y{"pad_y"};
    Input<int> res{"res"};                    // 1: full size, 2: half (Glare Res)
    Input<float> scale{"scale"};              // 1 / the transform's size
    Input<float> hue{"hue"}, saturation{"saturation"};
    Input<int> blur{"blur"};                  // Blur Glare: whole pixels
    Input<float> blur_frac{"blur_frac"};
    Input<float> source_opacity{"source_opacity"}, under{"under"}, affect_alpha{"affect_alpha"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func glare("glare"), turned("turned");
        // The glare at this pixel: at half size, between the 4 nearest of the half image.
        Expr u = (cast<float>(x) + 0.5f) / cast<float>(res) - 0.5f;
        Expr v = (cast<float>(y) + 0.5f) / cast<float>(res) - 0.5f;
        Expr ux = cast<int>(floor(u)), vy = cast<int>(floor(v));
        Expr fu = u - floor(u), fv = v - floor(v);
        Expr nx = data.width(), ny = data.height();
        auto sample = [&](Expr i, Expr j) {
            return data(clamp(i + pad_x, 0, nx - 1), clamp(j + pad_y, 0, ny - 1), clamp(c, 0, 2));
        };
        Expr bilinear = (sample(ux, vy) * (1.0f - fu) + sample(ux + 1, vy) * fu) * (1.0f - fv) +
                        (sample(ux, vy + 1) * (1.0f - fu) + sample(ux + 1, vy + 1) * fu) * fv;
        // (at least 0: the transforms leave crumbs around 0, which Hue Shift would amplify)
        glare(x, y, c) =
            select(c < 3, max(select(res == 1, sample(x, y), bilinear) * scale, 0.0f), 0.0f);
        Expr shifted = ms::hue_shifted(glare(x, y, 0), glare(x, y, 1), glare(x, y, 2), c, hue,
                                       glare(x, y, c));
        Func hued("hued");
        hued(x, y, c) = shifted;
        Expr luma = 0.3125f * hued(x, y, 0) + 0.5f * hued(x, y, 1) + 0.1875f * hued(x, y, 2);
        turned(x, y, c) = select(c < 3, luma + (hued(x, y, c) - luma) * saturation, 0.0f);
        std::vector<ms_blur::Box> boxes;
        Func blurred = ms_blur::blur3_subpixel(turned, blur, blur_frac, Expr(0), source.width(),
                                               source.height(), gpu, "gblur", boxes);
        Func ray("ray");
        ray(x, y, c) = blurred(x, y, c);
        // As Glint: screened over the Source x Source Opacity, or under it; Affect Alpha.
        output(x, y, c) = ms::screened(source, ray, x, y, c, source_opacity, under, affect_alpha);

        ms::interleaved(data);
        ms::interleaved(source);
        ms::interleaved(output);
        ms_blur::schedule(boxes, gpu, 4);
        for (Func fn : {glare, turned, ray})
            ms::schedule_float4(fn.compute_root(), gpu, 32, 8);
        ms::schedule_float4(output, gpu, 32, 8);
    }
};

HALIDE_REGISTER_GENERATOR(GlareKernel, ms_glare_kernel)
HALIDE_REGISTER_GENERATOR(GlareLabelInit, ms_glare_label_init)
HALIDE_REGISTER_GENERATOR(GlareLabelRound, ms_glare_label_round)
HALIDE_REGISTER_GENERATOR(GlareSpots, ms_glare_spots)
HALIDE_REGISTER_GENERATOR(GlareHalf, ms_glare_half)
HALIDE_REGISTER_GENERATOR(GlareFinish, ms_glare_finish)
