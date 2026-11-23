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
//                         x Scale Colors again (b), into the transform's image (on the GPU
//                         without atomics: row by row, then along each region's rows)
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
        const bool gpu = get_target().has_gpu_feature();
        Expr w = labels.width(), h = labels.height();
        // (on the GPU, the edges repeated: Halide reads past them in its blocks of rows)
        lab = gpu ? BoundaryConditions::repeat_edge(labels) : Func(labels);
        glint = gpu ? BoundaryConditions::repeat_edge(glints) : Func(glints);
        // Each region's sums: colour (0..2), weight (3), weighted offsets from its first pixel
        // (4, 5: kept small for the floats), first summed along each row's runs of one label.
        Expr lv = lab(x, y);
        Expr lvi = cast<int>(min(lv, cast<uint32_t>(w * h - 1)));
        Expr weight = glint(x, y, 0) + glint(x, y, 1) + glint(x, y, 2);
        values(x, y, k) =
            select(lv == none(), 0.0f, k < 3, glint(x, y, clamp(k, 0, 2)), k == 3, weight, k == 4,
                   weight * cast<float>(x - lvi % w), weight * cast<float>(y - lvi / w));
        if (gpu)
            on_gpu();
        else
            on_cpu();
        ms::interleaved(glints);
        ms::interleaved(impulses);
    }

  private:
    Var k{"k"}, i{"i"};
    Func lab{"lab"}, glint{"glint"}, values{"values"};

    // The strength of an impulse in channel ch, the region's colour sums s0..s2: min(m b,
    // cap) / norm with m its largest channel, its colour kept, times b again.
    Expr strength(Expr s0, Expr s1, Expr s2, Expr ch) {
        auto bc = [&](Expr n) { return select(n == 0, bc_r, n == 1, bc_g, bc_b); };
        Expr e0 = s0 * bc_r, e1 = s1 * bc_g, e2 = s2 * bc_b;
        Expr m = max(e0, max(e1, e2));
        Expr ec = select(ch == 0, e0, ch == 1, e1, e2);
        return select(m > 0.0f, min(m, cap) / norm * (ec / m) * bc(ch), 0.0f);
    }

    // The CPU: the runs (a scan along each row, restarting where the label changes), each
    // run's last pixel adding its run to the region's sums at its first pixel, then one
    // impulse per region; one pass over the image each, in order.
    void on_cpu() {
        Expr w = labels.width(), h = labels.height();
        Func runs("runs"), sums("sums");
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
        Expr s = strength(sums(q.x, q.y, 0), sums(q.x, q.y, 1), sums(q.x, q.y, 2), q.w);
        Expr nx = impulses.width(), ny = impulses.height();
        impulses(clamp(ix + pad_x, 0, nx - 1), clamp(iy + pad_y, 0, ny - 1), q.w) += corner * s;

        sums.compute_root().bound(k, 0, 6).parallel(y);
        sums.update().unscheduled();
        runs.compute_root().bound(k, 0, 6).reorder(k, x, y).parallel(y);
        runs.update().parallel(y);
        impulses.bound(c, 0, 4).parallel(y);
        impulses.update().unscheduled();
    }

    // The GPU, without atomics (none in Metal and Direct3D 12 code; the same order on every
    // API), each stage gathering or writing its own row only: each row's runs listed (label,
    // sums; at most (width + 1) / 2); in each row the first run of a label (its head) sums the
    // label's runs there and finds the label's head in the next row (a region's rows are
    // contiguous); the head of a region's first pixel follows these links, a row at a time,
    // to the region's sums; the head in the row of the centroid holds the impulse, added by
    // one thread per row (the lower corners into its row, then the upper ones into the next).
    void on_gpu() {
        Expr w = labels.width(), h = labels.height(), slots = (w + 1) / 2;
        Func ends("ends"), before("before"), count("count"), run_label("run_label"),
            run_sums("run_sums");
        Expr lv = lab(x, y);
        ends(x, y) = lv != none() && (x == w - 1 || lab(min(x + 1, w - 1), y) != lv);
        // The run of a pixel: how many runs end before it in its row.
        before(x, y) = 0;
        RDom rx(1, max(w - 1, 1), "rx");
        before(rx, y) = before(rx - 1, y) + select(ends(rx - 1, y), 1, 0);
        count(y) = before(w - 1, y) + select(ends(w - 1, y), 1, 0);
        run_label(i, y) = none();
        RDom pe(0, w, "pe");
        pe.where(ends(pe, y));
        run_label(clamp(before(pe, y), 0, slots - 1), y) = lab(pe, y);
        run_sums(i, y, k) = 0.0f;
        RDom pl(0, w, "pl");
        pl.where(lab(pl, y) != none());
        run_sums(clamp(before(pl, y), 0, slots - 1), y, k) += values(pl, y, k);

        // The heads (no run of their label before them in the row), their row's sums and the
        // next row's head (slots: none).
        Func dup("dup"), group("group"), link("link");
        Expr n = count(y), label = run_label(i, y);
        dup(i, y) = 0;
        RDom ja(0, slots, "ja");
        ja.where(ja < min(i, n));
        dup(i, y) += select(run_label(ja, y) == label, 1, 0);
        // (the loops' ends as bounds: Halide trims the loops to them)
        Expr head = i < n && dup(i, y) == 0;
        group(i, y, k) = 0.0f;
        RDom jb(0, slots, "jb");
        jb.where(jb >= i && jb < select(head, n, 0));
        group(i, y, k) += select(run_label(jb, y) == label, run_sums(jb, y, k), 0.0f);
        Expr up = min(y + 1, h - 1);
        link(i, y) = slots;
        RDom jc(0, slots, "jc");
        jc.where(jc < select(head && y + 1 < h, count(up), 0));
        link(i, y) = min(link(i, y), select(run_label(jc, up) == label, jc, slots));

        // The region's sums at the head of its first pixel: (sums, head) along the links (the
        // head last: Halide stores a tuple's values in order, the sums read the one before).
        Func chase("chase");
        std::vector<Expr> start(6, 0.0f), next;
        start.push_back(i);
        chase(i, y) = Tuple(start);
        RDom r(0, h, "r");
        Expr at = chase(i, y)[6];
        Expr first = head && label / cast<uint32_t>(w) == cast<uint32_t>(y);
        r.where(r < select(first, h - y, 0) && at < slots);
        Expr row = min(y + r, h - 1), ai = clamp(at, 0, slots - 1);
        for (int s = 0; s < 6; ++s)
            next.push_back(chase(i, y)[s] + group(ai, row, s));
        next.push_back(link(ai, row));
        chase(i, y) = Tuple(next);

        // A head's impulse (its column, or -1; fx, fy; the 3 strengths): the head in the row
        // of the centroid (or in the last row, should the rounding put the centroid past it).
        Func spot("spot");
        Expr li = cast<int>(min(label, cast<uint32_t>(w * h - 1)));
        Expr x0 = clamp(li % w, 0, w - 1), y0 = clamp(li / w, 0, h - 1);
        Expr i0 = clamp(before(x0, y0), 0, slots - 1);
        auto sum = [&](int s) { return chase(i0, y0)[s]; };
        Expr total = sum(3);
        Expr cx = cast<float>(x0) + sum(4) / total, cy = cast<float>(y0) + sum(5) / total;
        Expr ky = cast<int>(floor(cy));
        Expr here = head && total > 0.0f && (ky == y || (ky > y && link(i, y) == slots));
        spot(i, y) = Tuple(select(here, cast<int>(floor(cx)), -1), cx - floor(cx), cy - floor(cy),
                           strength(sum(0), sum(1), sum(2), 0), strength(sum(0), sum(1), sum(2), 1),
                           strength(sum(0), sum(1), sum(2), 2));

        // The impulses: by the thread of the head's row, its corners below (into that row),
        // then above (into the next; the transform's image is larger than the image: no two
        // rows clamped together).
        impulses(x, y, c) = 0.0f;
        Expr nx = impulses.width(), ny = impulses.height();
        std::vector<RDom> d;
        for (int above = 0; above < 2; ++above) {
            d.push_back(RDom(0, 2, 0, 3, 0, slots, 0, h, "d")); // corner, channel, run, row
            RDom q = d.back();
            q.where(q.z < count(q.w) && spot(q.z, q.w)[0] >= 0);
            Expr fx = spot(q.z, q.w)[1], fy = spot(q.z, q.w)[2];
            Expr s =
                select(q.y == 0, spot(q.z, q.w)[3], q.y == 1, spot(q.z, q.w)[4], spot(q.z, q.w)[5]);
            Expr corner = select(q.x == 1, fx, 1.0f - fx) * (above ? fy : 1.0f - fy);
            impulses(clamp(spot(q.z, q.w)[0] + q.x + pad_x, 0, nx - 1),
                     clamp(q.w + above + pad_y, 0, ny - 1), q.y) += corner * s;
        }

        // A thread per row for the scans along rows and the impulses; tiles of threads (run,
        // row) for the rest.
        const TailStrategy guard = TailStrategy::GuardWithIf;
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi"), ro("ro"), ri("ri");
        RVar rqo("rqo"), rqi("rqi");
        auto tiles = [&](Stage st, Var a) { st.gpu_tile(a, y, xo, yo, xi, yi, 32, 8, guard); };
        auto rows = [&](Stage st) {
            st.split(y, ro, ri, 64, guard).gpu_blocks(ro).gpu_threads(ri);
        };
        before.compute_root();
        tiles(before, x);
        rows(before.update());
        run_label.compute_root();
        tiles(run_label, i);
        rows(run_label.update());
        for (Func f : {run_sums, group}) {
            f.compute_root().bound(k, 0, 6).reorder_storage(k, i, y);
            tiles(f.reorder(k, i, y).unroll(k), i);
        }
        rows(run_sums.update().reorder(k, pl, y).unroll(k));
        tiles(group.update().reorder(k, jb, i, y).unroll(k), i);
        for (Func f : {dup, link, chase, spot}) {
            tiles(f.compute_root(), i);
            if (f.has_update_definition())
                tiles(f.update(), i);
        }
        impulses.bound(c, 0, 4).reorder(c, x, y).vectorize(c);
        tiles(impulses, x);
        for (int above = 0; above < 2; ++above)
            impulses.update(above)
                .allow_race_conditions()
                .unroll(d[above].x)
                .unroll(d[above].y)
                .split(d[above].w, rqo, rqi, 64, guard)
                .gpu_blocks(rqo)
                .gpu_threads(rqi);
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
