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
#include "glare_layout.h"
#include "hue.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c"), e("e");

const float PI = 3.14159265358979f;
// The label of a pixel in no spot (the largest).
Expr none() { return Internal::UIntImm::make(UInt(32), 0xffffffffu); }

Expr smoothstep(Expr t) {
    t = clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// erf(x), Abramowitz and Stegun 7.1.26 (error 1.5e-7).
Expr erf_approx(Expr v) {
    Expr a = abs(v);
    Expr t = 1.0f / (1.0f + 0.3275911f * a);
    Expr p = ((((1.061405429f * t - 1.453152027f) * t + 1.421413741f) * t - 0.284496736f) * t +
              0.254829592f) *
             t;
    Expr r = 1.0f - p * exp(-a * a);
    return select(v < 0.0f, -r, r);
}

// A band's edge at distance d (pixels) outside it.
Expr band_edge(Expr d) { return 1.0f - smoothstep((d + 0.35f) / 2.2f); }

// The schedule of a whole-image stage: float4 pixels, tiles of threads.
void tiles(Func f, bool gpu) {
    Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
    f.bound(c, 0, 4).reorder(c, x, y);
    if (gpu)
        f.vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
    else
        f.unroll(c).vectorize(x, 8).parallel(y);
}

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
        // Fields read clamped: Halide's bounds of the computed ones (stops) would be unbounded.
        auto field = [&](Expr i, Expr el) {
            return rows(clamp(i, 0, GLARE_FIELDS - 1), clamp(el, 0, GLARE_MAX_ELEMENTS - 1));
        };
        auto f = [&](int i) { return field(i, e); };
        Expr kind = f(GLARE_KIND);
        Expr rot = f(GLARE_ROTATE);
        Expr sx = dx / f(GLARE_SCALE_X), sy = dy / f(GLARE_SCALE_Y);
        Expr ex = sx * cos(rot) + sy * sin(rot), ey = -sx * sin(rot) + sy * cos(rot);
        Expr r0 = sqrt(ex * ex + ey * ey);
        Expr th = atan2(ey, ex);
        // The angles of sectors and waves, and the rays' width: without the stretch.
        Expr ax = dx / f(GLARE_ANGLE_X), ay = dy / f(GLARE_ANGLE_Y);
        Expr aex = ax * cos(rot) + ay * sin(rot), aey = -ax * sin(rot) + ay * cos(rot);
        Expr angle = atan2(aey, aex);
        Expr gain = select(c == 0, f(GLARE_GAIN_R), c == 1, f(GLARE_GAIN_G), f(GLARE_GAIN_B));

        // FALLOFF: (1 - t)^1.965 exp(-0.475 t), inside its circle.
        Expr tb = r0 / f(GLARE_FALLOFF_R);
        Expr clip_r = sqrt(dx * dx + dy * dy) / f(GLARE_CLIP_SCALE);
        Expr falloff = pow(max(1.0f - tb, 0.0f), 1.965f) * exp(-0.475f * tb) *
                       band_edge(clip_r - f(GLARE_CLIP_R));

        // STOPS: the radius through the polygon and the radius waves first.
        Expr start = f(GLARE_START);
        Expr sides = f(GLARE_SIDES);
        Expr seg = 2.0f * PI / max(sides, 3.0f);
        Expr phase = th - start;
        Expr phi = phase - seg * floor(phase / seg) - seg / 2.0f;
        Expr edge = cos(PI / max(sides, 3.0f)) / cos(phi);
        edge = edge + (1.0f - edge) * f(GLARE_ROUNDNESS);
        Expr r1 = select(sides >= 3.0f, r0 / edge, r0);
        Expr lo = f(GLARE_RW_MIN), hi = f(GLARE_RW_MAX);
        // (their frequency across the sector's width; a whole turn: per turn)
        Expr whole = phase - 2.0f * PI * floor(phase / (2.0f * PI));
        Expr wave_c =
            0.5f + 0.5f * cos(f(GLARE_RW_FREQ) * whole * (2.0f * PI / f(GLARE_WIDTH)));
        Expr r = select(lo != 1.0f || hi != 1.0f,
                        r1 / (lo + (hi - lo) * pow(max(wave_c, 0.0f), f(GLARE_RW_SPIKE))), r1);
        Expr R = f(GLARE_R);
        Expr t = r / R;
        // The colour: linear between the stops (held beyond them), in its band.
        Expr stops = f(GLARE_STOP_COUNT);
        auto radius = [&](Expr k) { return field(GLARE_STOP_RADII + k, e); };
        auto colour = [&](Expr k) { return field(GLARE_STOP_COLORS + 3 * k + c, e); };
        Expr last = cast<int>(stops) - 1;
        RDom k(0, GLARE_MAX_STOPS - 1, "k");
        k.where(k < last);
        Expr ka = radius(k), kb = radius(k + 1);
        Expr inside = t >= ka && t < kb && kb > ka;
        Expr between = sum(select(inside, colour(k) + (colour(k + 1) - colour(k)) *
                                                          ((t - ka) / select(kb > ka, kb - ka, 1.0f)),
                                  0.0f));
        Expr held = select(t < radius(0), colour(0), t >= radius(last), colour(last), between);
        auto edge_of = [&](Expr d) {
            return select(f(GLARE_CRISP) != 0.0f, clamp(0.57f - d, 0.0f, 1.0f), band_edge(d));
        };
        Expr outer = edge_of(r - radius(last) * R);
        Expr inner = select(radius(0) > 0.0f, edge_of(radius(0) * R - r), 1.0f);
        // Blur Gradient: that profile (0 outside the band) blurred by a gaussian of sigma: its
        // jumps through Phi, its slope changes through x Phi(x / sigma) + sigma phi(x / sigma).
        Expr sigma = f(GLARE_SIGMA), sig = select(sigma > 0.0f, sigma, 1.0f);
        auto Phi = [&](Expr d) { return 0.5f * (1.0f + erf_approx(d / (sig * 1.41421356f))); };
        auto ramp = [&](Expr d) {
            return d * Phi(d) + sig * exp(-0.5f * (d / sig) * (d / sig)) * 0.39894228f;
        };
        auto knot = [&](Expr i) { return radius(i) * R; };
        auto slope = [&](Expr i) { // of the segment from stop i to i + 1
            Expr gap = knot(i + 1) - knot(i);
            Expr rise = (colour(i + 1) - colour(i)) / select(gap > 0.0f, gap, 1.0f);
            return select(i >= 0 && gap > 0.0f, rise, 0.0f);
        };
        RDom q(0, GLARE_MAX_STOPS - 1, "q");
        q.where(q < last);
        Expr bends = sum((slope(q) - slope(q - 1)) * ramp(r - knot(q)));
        Expr blurred = select(radius(0) > 0.0f, colour(0) * Phi(r - knot(0)), colour(0)) + bends -
                       slope(last - 1) * ramp(r - knot(last)) - colour(last) * Phi(r - knot(last));
        Expr disc = select(r <= radius(0) * R, colour(0), 0.0f); // one stop
        Expr col = select(stops < 2.0f, disc, sigma > 0.0f, blurred, held * outer * inner);
        col = col * (1.0f - f(GLARE_FADE) * clamp(t, 0.0f, 1.0f));
        // Sectors: the angular window (repeated), soft edges.
        Expr width = f(GLARE_WIDTH), soft = f(GLARE_SOFTNESS);
        Expr aphase = angle - start;
        Expr turn = aphase - 2.0f * PI * floor(aphase / (2.0f * PI));
        Expr u = turn / width;
        Expr v = u * f(GLARE_REPEATS) - floor(u * f(GLARE_REPEATS));
        // Sharp edges (no softness): the start edge itself left out, the end edge over one
        // pixel of arc.
        Expr past = (turn - width) * r0;
        Expr sharp = select(u <= 1e-5f, 0.0f, u <= 1.0f, 1.0f, clamp(0.57f - past, 0.0f, 1.0f));
        Expr window = select(soft > 0.0f,
                             smoothstep(min(v, 1.0f - v) / (0.5f * soft)) *
                                 select(u <= 1.0f, 1.0f, 0.0f),
                             sharp);
        window = select(width >= 2.0f * PI - 1e-5f && soft == 0.0f, 1.0f, window);
        Expr wave = f(GLARE_WAVE), ww = f(GLARE_WAVE_WIDTH);
        Expr ph = (u - 0.5f) * f(GLARE_WAVE_FREQ) + 0.5f;
        ph = ph - floor(ph) - 0.5f;
        Expr pulse = 1.0f - smoothstep(abs(ph) / (0.5f * select(ww > 0.0f, ww, 1.0f)));
        Expr waves = select(wave == 0.0f, 1.0f, ww >= 1.0f, 1.0f + wave, 1.0f + wave * pulse);
        Expr gradient = col * select(f(GLARE_SECTOR) != 0.0f, window, 1.0f) * f(GLARE_BOOST) * waves;

        // RAYS: lines through the centre, 1 - d / L along (stretched), 1 - smoothstep(d / w)
        // across (not stretched); cut at L unstretched.
        Expr L = f(GLARE_R), w = f(GLARE_RAY_WIDTH), lines = f(GLARE_LINES);
        RDom j(0, GLARE_MAX_LINES, "j");
        j.where(cast<float>(j) < lines);
        Expr ang = rot + cast<float>(j) * PI / select(lines > 0.0f, lines, 1.0f);
        Expr along = sx * cos(ang) + sy * sin(ang), across = -ax * sin(ang) + ay * cos(ang);
        Expr safe_w = select(w > 0.0f, w, 1.0f); // no width: no rays
        Expr ray = select(w > 0.0f,
                          sum(max(1.0f - abs(along) / L, 0.0f) *
                              (1.0f - smoothstep(abs(across) / safe_w))),
                          0.0f) *
                   band_edge(clip_r - f(GLARE_CLIP_R));

        Func element("element");
        element(x, y, c, e) = select(kind == GLARE_FALLOFF, falloff,
                                     kind == GLARE_RAYS, ray, gradient) * gain;
        // The sum of the elements, each where it reaches (its radius x its stretch, x 1.6
        // for the radius waves and blurred edges).
        Func sum_of("sum_of");
        RDom el(0, count, "el");
        Expr reach = field(GLARE_R, el) * 1.6f *
                         max(field(GLARE_SCALE_X, el), field(GLARE_SCALE_Y, el)) +
                     4.0f * field(GLARE_SIGMA, el) + 4.0f;
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
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        if (gpu)
            labels.gpu_tile(x, y, xo, yo, xi, yi, 32, 8);
        else
            labels.vectorize(x, 8).parallel(y);
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
        tiles(half, gpu);
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
        Func blurred = ms_blur::blur3_subpixel(turned, blur, blur_frac, blur, blur, blur_frac, blur,
                                               Expr(0), source.width(), source.height(), gpu,
                                               "gblur", boxes);
        Func ray("ray");
        ray(x, y, c) = blurred(x, y, c);
        // As Glint: screened over the Source x Source Opacity, or under it; Affect Alpha.
        Expr a = source(x, y, 3) * source_opacity;
        Expr s = source(x, y, c) * source_opacity;
        Expr rc = ray(x, y, c);
        Expr s0 = max(s, 0.0f);
        Expr on = max(s0 + rc * max(1.0f - s0, 0.0f), rc);
        Expr below = s + (1.0f - a) * rc;
        Expr rgb = on + (below - on) * under;
        Expr glare_alpha = max(ray(x, y, 0), max(ray(x, y, 1), ray(x, y, 2))) * affect_alpha;
        output(x, y, c) = select(c == 3, a + (1.0f - a) * glare_alpha, rgb);

        ms::interleaved(data);
        ms::interleaved(source);
        ms::interleaved(output);
        ms_blur::schedule(boxes, gpu, 4);
        for (Func fn : {glare, turned, ray})
            tiles(fn.compute_root(), gpu);
        tiles(output, gpu);
    }
};

HALIDE_REGISTER_GENERATOR(GlareKernel, ms_glare_kernel)
HALIDE_REGISTER_GENERATOR(GlareLabelInit, ms_glare_label_init)
HALIDE_REGISTER_GENERATOR(GlareLabelRound, ms_glare_label_round)
HALIDE_REGISTER_GENERATOR(GlareSpots, ms_glare_spots)
HALIDE_REGISTER_GENERATOR(GlareHalf, ms_glare_half)
HALIDE_REGISTER_GENERATOR(GlareFinish, ms_glare_finish)
