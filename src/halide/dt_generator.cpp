// MS_EdgeAwareBlur in Halide: the Domain Transform filter (Gastal & Oliveira, SIGGRAPH
// 2011). Each blur type is one pass, horizontal then vertical, which
// effects/ms_edgeawareblur.c calls once per pass of the Filter (1, 2, 3 or 5); plus the
// distances, and Soft's weights. On the CPU, in double; Sharp and Softer also on the GPU,
// in float (see "GPU" below).
//
// Images are packed RGBA floats (x, y, c), interleaved. dx / dy: the distance from each
// pixel to its left / lower neighbour in the transformed domain (planar).
//
// CPU speed: each step of a line works on the 4 channels at once (one vector), and several
// lines go side by side, so the CPU always has independent work while a line's recursion
// waits on its previous step. Lines are processed in tiles that stay in cache.

#include "Halide.h"
#include "blur.h"
#include "mask.h"

#include <cmath>
#include <functional>
#include <memory>

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

constexpr int ROWS = 8;     // rows filtered side by side (horizontal)
constexpr int COLUMNS = 32; // columns filtered side by side (vertical)

} // namespace

// The distance from each pixel to its left (dx) and lower (dy) neighbour in the
// transformed domain: 1 / max(m, mask_floor) + scale x contrast.
// contrast = sum of |RGB difference| of the edge image, or 0.75 x sum of |RGBA difference|.
// m = the smaller mask value (mask.h) of the two pixels (has_mask = 0: m = 1, `mask` is not
// read).
class DtDistances : public Generator<DtDistances> {
  public:
    Input<Buffer<float, 3>> edge{"edge"};
    Input<Buffer<float, 3>> mask{"mask"};
    Input<int> has_mask{"has_mask"}, mask_alpha{"mask_alpha"}, invert_mask{"invert_mask"};
    Input<int> use_alpha{"use_alpha"};
    Input<float> scale_x{"scale_x"}, scale_y{"scale_y"};
    Input<float> floor_x{"floor_x"}, floor_y{"floor_y"};
    Output<Buffer<float, 2>> dx{"dx"}, dy{"dy"};

    void generate() {
        auto contrast = [&](Expr x0, Expr y0, Expr x1, Expr y1) {
            Expr rgb = abs(edge(x0, y0, 0) - edge(x1, y1, 0)) + abs(edge(x0, y0, 1) - edge(x1, y1, 1)) +
                       abs(edge(x0, y0, 2) - edge(x1, y1, 2));
            return select(use_alpha != 0, 0.75f * (rgb + abs(edge(x0, y0, 3) - edge(x1, y1, 3))), rgb);
        };
        Func m_value("m_value");
        m_value(x, y) = ms_mask::value(mask, x, y, mask_alpha, invert_mask);
        auto stretch = [&](Expr x0, Expr y0, Expr x1, Expr y1, Expr floor_value) {
            Expr m = min(m_value(x0, y0), m_value(x1, y1));
            return select(has_mask != 0, 1.0f / max(m, floor_value), 1.0f);
        };
        Expr xl = max(x - 1, 0), yb = max(y - 1, 0);
        dx(x, y) = select(x == 0, 1.0f, stretch(x, y, xl, y, floor_x) + scale_x * contrast(x, y, xl, y));
        dy(x, y) = select(y == 0, 1.0f, stretch(x, y, x, yb, floor_y) + scale_y * contrast(x, y, x, yb));

        edge.dim(0).set_stride(4);
        edge.dim(2).set_stride(1).set_bounds(0, 4);
        mask.dim(0).set_stride(4);
        mask.dim(2).set_stride(1).set_bounds(0, 4);

        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        for (auto *d : {&dx, &dy}) {
            if (get_target().has_gpu_feature())
                (*d).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            else
                (*d).split(y, yo, yi, 16).parallel(yo).vectorize(x, 8);
        }
    }
};

// Edge Smooth: the edge image blurred by the 3-box blur of the effects (blur.h); on the GPU,
// its prefix boxes (the same cost whatever the radius).
class DtEdgeSmooth : public Generator<DtEdgeSmooth> {
  public:
    Input<Buffer<float, 3>> edge{"edge"};
    Input<int> radius{"radius"}; // total radius in pixels, rounded
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func in("in");
        in(x, y, c) = edge(x, y, c);
        std::vector<ms_blur::Box> boxes;
        Func blurred = ms_blur::blur3(in, radius, radius, edge.width(), edge.height(), gpu,
                                      "smooth", boxes, false, true);
        output(x, y, c) = blurred(x, y, c);

        edge.dim(0).set_stride(4);
        edge.dim(2).set_stride(1).set_bounds(0, 4);
        output.dim(0).set_stride(4);
        output.dim(2).set_stride(1).set_bounds(0, 4);
        ms_blur::schedule(boxes, gpu, 4);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        output.bound(c, 0, 4).reorder(c, x, y);
        if (gpu)
            output.vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            output.unroll(c).vectorize(x, 8).parallel(y);
    }
};

// The largest mask value (mask.h) of the Mask, at least 1: the distances are at least its
// inverse.
class DtLargest : public Generator<DtLargest> {
  public:
    Input<Buffer<float, 3>> mask{"mask"};
    Input<int> mask_alpha{"mask_alpha"}, invert_mask{"invert_mask"};
    Output<Buffer<float, 1>> largest{"largest"}; // one value

    void generate() {
        RDom rx(0, mask.width(), "rx"), ry(0, mask.height(), "ry");
        Func row("row");
        row(y) = maximum(ms_mask::value(mask, rx, y, mask_alpha, invert_mask));
        largest(x) = max(1.0f, maximum(row(ry)));

        mask.dim(0).set_stride(4);
        mask.dim(2).set_stride(1).set_bounds(0, 4);
        Var yo("yo"), yi("yi");
        row.compute_root().split(y, yo, yi, 16).parallel(yo);
    }
};

// Soft Edges, the weights a^d of every pixel to its previous neighbour, per direction
// (a = exp(-sqrt(2) / sigma); ln(a) very negative: no blur that way). Computed once per
// filter, used by every pass.
class DtSoftWeights : public Generator<DtSoftWeights> {
  public:
    Input<Buffer<float, 2>> dx{"dx"}, dy{"dy"};
    Input<float> log_a_x{"log_a_x"}, log_a_y{"log_a_y"};
    Output<Buffer<float, 2>> wx{"wx"}, wy{"wy"};

    void generate() {
        wx(x, y) = exp(log_a_x * dx(x, y));
        wy(x, y) = exp(log_a_y * dy(x, y));
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        for (auto *w : {&wx, &wy}) {
            if (get_target().has_gpu_feature())
                (*w).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            else
                (*w).split(y, yo, yi, 16).parallel(yo).vectorize(x, 8);
        }
    }
};

// Soft Edges, one pass: the recursive filter, forward then backward along each row, then
// along each column, with the weights of DtSoftWeights.
class DtSoft : public Generator<DtSoft> {
  public:
    Input<Buffer<float, 3>> input{"input"};
    Input<Buffer<float, 2>> wx{"wx"}, wy{"wy"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        Expr width = input.width(), height = input.height();
        // Reads clamped to the image: the last tile or strip can be smaller than its
        // schedule's size (the extra lines are computed and never written).
        Func in = BoundaryConditions::repeat_edge(input);
        Func wxc = BoundaryConditions::repeat_edge(wx), wyc = BoundaryConditions::repeat_edge(wy);

        // Horizontal: forward then backward along each row.
        Func fx("fx"), bx("bx");
        RDom rx(1, width - 1, "rx");
        fx(x, y, c) = in(x, y, c);
        fx(rx, y, c) = (1 - wxc(rx, y)) * in(rx, y, c) + wxc(rx, y) * fx(rx - 1, y, c);
        RDom rbx(0, width - 1, "rbx");
        Expr ix = width - 2 - rbx;
        bx(x, y, c) = fx(x, y, c);
        bx(ix, y, c) = (1 - wxc(ix + 1, y)) * fx(ix, y, c) + wxc(ix + 1, y) * bx(ix + 1, y, c);

        // Vertical: forward then backward along each column.
        Func fy("fy"), by("by");
        RDom ry(1, height - 1, "ry");
        fy(x, y, c) = bx(x, y, c);
        fy(x, ry, c) = (1 - wyc(x, ry)) * bx(x, ry, c) + wyc(x, ry) * fy(x, ry - 1, c);
        RDom rby(0, height - 1, "rby");
        Expr iy = height - 2 - rby;
        by(x, y, c) = fy(x, y, c);
        by(x, iy, c) = (1 - wyc(x, iy + 1)) * fy(x, iy, c) + wyc(x, iy + 1) * by(x, iy + 1, c);
        output(x, y, c) = by(x, y, c);

        input.dim(0).set_stride(4);
        input.dim(2).set_stride(1).set_bounds(0, 4);
        output.dim(0).set_stride(4);
        output.dim(2).set_stride(1).set_bounds(0, 4);

        // ---- schedule
        Var xo("xo"), xi("xi"), yo("yo"), yi("yi"), cx("cx");
        // Horizontal: tiles of ROWS rows, in parallel. Each step along x does the tile's
        // rows x 4 channels (one vector per row).
        bx.compute_root().bound(c, 0, 4).reorder_storage(c, x, y);
        bx.split(y, yo, yi, ROWS, TailStrategy::GuardWithIf).reorder(c, x, yi, yo).vectorize(c)
            .parallel(yo);
        bx.update().split(y, yo, yi, ROWS, TailStrategy::GuardWithIf).reorder(c, yi, rbx, yo)
            .vectorize(c).unroll(yi).parallel(yo);
        fx.compute_at(bx, yo).bound(c, 0, 4).reorder_storage(c, x, y);
        fx.reorder(c, x, y).vectorize(c);
        fx.update().reorder(c, y, rx).vectorize(c);

        // Vertical: strips of COLUMNS columns, in parallel, each filtered in a small buffer
        // that stays in cache, then copied to the output. Each step along y does the
        // strip's columns x 4 channels: contiguous in memory, one wide vector.
        // (Vector tails are guarded: the last strip can be narrower than COLUMNS.)
        const TailStrategy guard = TailStrategy::GuardWithIf;
        for (Func *f : {&fy, &by}) {
            f->compute_at(output, xo).bound(c, 0, 4).reorder_storage(c, x, y);
            f->reorder(c, x, y).fuse(c, x, cx).vectorize(cx, 16, guard);
        }
        fy.update().reorder(c, x, ry).fuse(c, x, cx).vectorize(cx, 16, guard);
        by.update().reorder(c, x, rby).fuse(c, x, cx).vectorize(cx, 16, guard);
        output.split(x, xo, xi, COLUMNS, TailStrategy::GuardWithIf).reorder(c, xi, y, xo)
            .fuse(c, xi, cx).vectorize(cx, 16, guard).parallel(xo);
    }
};

// Sharp Edges, one pass: normalised convolution, along
// each row, then along each column. For each sample, the mean of the samples whose
// position ct (running sum of the distances) is within [ct - r, ct + r]; a fractional
// radius blends it with the window extended by one sample below (Subpixel).
//
// The window edges are found by binary search: a distance is at least 1 / (largest mask
// value), so a window holds at most `span` samples per side (computed by the caller), and
// log2(span) halvings find each edge. Sums are in double, in the same order as the C
// version (same results).
class DtSharp : public Generator<DtSharp> {
  public:
    Input<Buffer<float, 3>> input{"input"};
    Input<Buffer<float, 2>> dx{"dx"}, dy{"dy"};
    Input<double> radius_x{"radius_x"}, radius_y{"radius_y"}; // 0: no blur that way
    Input<int> span_x{"span_x"}, span_y{"span_y"};             // samples per side, at most
    Input<int> span_wide{"span_wide"}; // samples one more unit of distance can add, at most
    Output<Buffer<float, 3>> output{"output"};

    // Smallest index in [first, last] for which above(index) holds (false before it, true
    // from it on; it must hold at `last`), with `steps` halvings.
    // Smallest index in [first, last] for which above(index) holds (false before it, true
    // from it on; it must hold at `last`). Branch-free binary search: from `first`, jumps of
    // 2^(steps-1), ..., 2, 1 samples are kept while every sample skipped is still below.
    // steps = ceil(log2(last - first + 1)) is enough.
    struct Search {
        Func pos;
        RDom k;
    };
    static Search lower_bound(const std::string &name, Expr first, Expr last, Expr steps,
                              const std::function<Expr(Expr)> &above) {
        Func pos(name);
        pos(x, y) = first;
        RDom k(0, steps, name + "_k");
        Expr probe = pos(x, y) + (1 << (steps - 1 - k));
        pos(x, y) = select(probe <= last && !above(probe - 1), probe, pos(x, y));
        return {pos, k};
    }

    // ceil(log2(n)) for n >= 1, exact: the float logarithm, then corrected by one where it
    // is off (just above a power of two, the float rounds down).
    static Expr log2_ceil(Expr n) {
        Expr guess = max(cast<int>(ceil(log(cast<float>(n)) / std::log(2.0f))), 0);
        guess = select((1 << guess) < n, guess + 1, guess);
        return select(guess > 0 && (1 << (guess - 1)) >= n, guess - 1, guess);
    }

    // Filters `in` along x (along_x) or y with the distances `dist`.
    struct Line {
        Func out, ct, sums;
        RDom r, q; // along the line, for ct and sums
        std::vector<Search> searches;
    };
    Line filter(Func in, Func dist, Expr length, bool along_x, Expr radius, Expr span,
                const std::string &name) {
        Line l{Func(name), Func(name + "_ct"), Func(name + "_sums"),
               RDom(1, length - 1, name + "_r"), RDom(0, length, name + "_q"), {}};
        RDom &r = l.r, &q = l.q;
        Expr eps = Internal::make_const(Float(64), 1e-9); // window bounds are inclusive
        Var &u = along_x ? x : y;  // position along the line
        auto at = [&](Func f, Expr i) { return along_x ? f(i, y) : f(x, i); };
        auto at_c = [&](Func f, Expr i) { return along_x ? f(i, y, c) : f(x, i, c); };

        // Running sums: position ct (ct(0) = 0) and values (sums(i) = sum of values < i).
        l.ct(x, y) = cast<double>(0);
        if (along_x)
            l.ct(r, y) = l.ct(r - 1, y) + cast<double>(dist(r, y));
        else
            l.ct(x, r) = l.ct(x, r - 1) + cast<double>(dist(x, r));
        l.sums(x, y, c) = cast<double>(0);
        if (along_x)
            l.sums(q + 1, y, c) = l.sums(q, y, c) + cast<double>(in(q, y, c));
        else
            l.sums(x, q + 1, c) = l.sums(x, q, c) + cast<double>(in(x, q, c));

        auto ct = [&](Expr i) { return at(l.ct, clamp(i, 0, length - 1)); };
        auto sums = [&](Expr i) { return at_c(l.sums, clamp(i, 0, length)); };
        Expr whole = floor(radius), fraction = radius - whole;
        Expr steps = log2_ceil(span + 1); // ranges of span + 1 samples
        Expr here = ct(u);

        // Lower edge: first index with ct >= ct(u) - whole - eps.
        Search lo = lower_bound(name + "_lo", max(u - span, 0), u, steps,
                                [&](Expr j) { return ct(j) >= here - whole - eps; });
        // Upper edge: one before the first index with ct > ct(u) + whole + eps.
        Search hi = lower_bound(name + "_hi", u + 1, min(u + span + 1, length), steps,
                                [&](Expr j) { return j >= length || ct(j) > here + whole + eps; });
        // Lower edge of the wider window (fractional radius): at most span_wide samples
        // below the lower edge.
        Expr lo_edge = lo.pos(x, y);
        Search lo_wide = lower_bound(name + "_low", max(lo_edge - span_wide, 0), lo_edge,
                                     log2_ceil(span_wide + 1),
                                     [&](Expr j) { return ct(j) >= here - (whole + 1) - eps; });
        l.searches = {lo, hi, lo_wide};

        Expr a = lo.pos(x, y), b = hi.pos(x, y) - 1, w = lo_wide.pos(x, y);
        Expr top = sums(b + 1);
        Expr mean = (cast<double>(1) - fraction) / cast<double>(b - a + 1) * (top - sums(a));
        Expr wide = mean + fraction / cast<double>(b - w + 1) * (top - sums(w));
        Expr filtered = cast<float>(select(fraction > 0, wide, mean));
        l.out(x, y, c) = select(radius > 0, filtered, in(x, y, c));
        return l;
    }

    void generate() {
        Expr width = input.width(), height = input.height();
        // Reads clamped to the image: the last tile or strip can be smaller than its
        // schedule's size (the extra lines are computed and never written).
        Func in = BoundaryConditions::repeat_edge(input);
        Func dxc = BoundaryConditions::repeat_edge(dx), dyc = BoundaryConditions::repeat_edge(dy);

        Line h = filter(in, dxc, width, true, radius_x, span_x, "h");
        Line v = filter(h.out, dyc, height, false, radius_y, span_y, "v");
        output(x, y, c) = v.out(x, y, c);

        input.dim(0).set_stride(4);
        input.dim(2).set_stride(1).set_bounds(0, 4);
        output.dim(0).set_stride(4);
        output.dim(2).set_stride(1).set_bounds(0, 4);

        // ---- schedule
        Var xo("xo"), xi("xi"), yo("yo"), yi("yi");
        const TailStrategy guard = TailStrategy::GuardWithIf;
        // Rows: tiles of ROWS rows in parallel; the searches of a row are vectorised along it.
        h.out.compute_root().bound(c, 0, 4).reorder_storage(c, x, y);
        h.out.split(y, yo, yi, ROWS, guard).reorder(c, x, yi, yo).vectorize(c).parallel(yo);
        h.ct.compute_at(h.out, yo);
        h.sums.compute_at(h.out, yo).bound(c, 0, 4).reorder_storage(c, x, y);
        h.sums.update().reorder(c, y, h.q).vectorize(c);
        for (Search &s : h.searches) {
            s.pos.compute_at(h.out, yi).vectorize(x, 8, guard);
            s.pos.update().reorder(x, s.k).vectorize(x, 8, guard);
        }
        // Columns: strips of COLUMNS columns in parallel; sums and searches are vectorised
        // across the strip's columns.
        output.bound(c, 0, 4).split(x, xo, xi, COLUMNS, guard).reorder(c, xi, y, xo)
            .vectorize(c).parallel(xo);
        v.ct.compute_at(output, xo);
        v.ct.update().reorder(x, v.r).vectorize(x, 8, guard);
        v.sums.compute_at(output, xo).bound(c, 0, 4).reorder_storage(c, x, y);
        v.sums.update().reorder(c, x, v.q).vectorize(c);
        for (Search &s : v.searches) {
            s.pos.compute_at(output, y).vectorize(x, 8, guard);
            s.pos.update().reorder(x, s.k).vectorize(x, 8, guard);
        }
    }
};

// Softer Edges, one pass: interpolated convolution, along
// each row, then along each column. For each sample, the mean over [ct - h, ct + h] of the
// signal linearly interpolated between samples (constant beyond the ends), from the running
// integral `area`. The segments holding ct - h and ct + h are found by binary search, as in
// DtSharp. Sums are in double, in the same order as the earlier C version (same results).
class DtSofter : public Generator<DtSofter> {
  public:
    Input<Buffer<float, 3>> input{"input"};
    Input<Buffer<float, 2>> dx{"dx"}, dy{"dy"};
    Input<double> half_x{"half_x"}, half_y{"half_y"}; // h per direction; 0: no blur that way
    Input<int> span_x{"span_x"}, span_y{"span_y"};     // samples per side, at most
    Output<Buffer<float, 3>> output{"output"};

    struct Line {
        Func out, ct, area;
        RDom r;
        std::vector<DtSharp::Search> searches;
    };
    Line filter(Func in, Func dist, Expr length, bool along_x, Expr h, Expr span,
                const std::string &name) {
        Line l{Func(name), Func(name + "_ct"), Func(name + "_area"), RDom(1, length - 1, name + "_r"), {}};
        RDom &r = l.r;
        Var &u = along_x ? x : y;
        Expr half = Internal::make_const(Float(64), 0.5);
        auto at = [&](Func f, Expr i) { return along_x ? f(i, y) : f(x, i); };
        auto value = [&](Expr i) { return cast<double>(along_x ? in(i, y, c) : in(x, i, c)); };

        // Position ct (ct(0) = 0) and integral area (area(i): from ct(0) to ct(i)).
        l.ct(x, y) = cast<double>(0);
        l.area(x, y, c) = cast<double>(0);
        if (along_x) {
            l.ct(r, y) = l.ct(r - 1, y) + cast<double>(dist(r, y));
            l.area(r, y, c) = l.area(r - 1, y, c) +
                              cast<double>(dist(r, y)) * half * (value(r) + value(r - 1));
        } else {
            l.ct(x, r) = l.ct(x, r - 1) + cast<double>(dist(x, r));
            l.area(x, r, c) = l.area(x, r - 1, c) +
                              cast<double>(dist(x, r)) * half * (value(r) + value(r - 1));
        }
        auto ct = [&](Expr i) { return at(l.ct, clamp(i, 0, length - 1)); };
        auto area = [&](Expr i) {
            i = clamp(i, 0, length - 1);
            return along_x ? l.area(i, y, c) : l.area(x, i, c);
        };
        auto v = [&](Expr i) { return value(clamp(i, 0, length - 1)); };
        Expr steps = DtSharp::log2_ceil(span + 2);

        // The integral up to position t, with k the last index <= length - 2 whose ct <= t.
        auto integral = [&](Expr t, Expr k) {
            Expr u0 = t - ct(k), len = ct(k + 1) - ct(k);
            Expr v0 = v(k), v1 = v(k + 1);
            Expr inside = area(k) + u0 * v0 + half * u0 * u0 * (v1 - v0) / len;
            Expr before = (t - ct(0)) * v(0);
            Expr after = area(length - 1) + (t - ct(length - 1)) * v(length - 1);
            return select(length == 1 || t <= ct(0), before, t >= ct(length - 1), after, inside);
        };
        Expr here = ct(u);
        Expr low_t = here - h, high_t = here + h;
        // First index (from 1 on) whose ct > t; the segment is the one before it.
        auto above = [&](Expr t) {
            return [=](Expr j) { return j >= length - 1 || ct(j) > t; };
        };
        DtSharp::Search low = DtSharp::lower_bound(name + "_low", max(u - span, 1),
                                                   max(min(u + 1, length - 1), 1), steps, above(low_t));
        DtSharp::Search high = DtSharp::lower_bound(name + "_high", max(min(u, length - 1), 1),
                                                    max(min(u + span + 1, length - 1), 1), steps,
                                                    above(high_t));
        l.searches = {low, high};
        Expr k_low = low.pos(x, y) - 1, k_high = high.pos(x, y) - 1;
        Expr mean = (integral(high_t, k_high) - integral(low_t, k_low)) / (2 * h);
        l.out(x, y, c) = select(h > 0, cast<float>(mean), in(x, y, c));
        return l;
    }

    void generate() {
        Expr width = input.width(), height = input.height();
        Func in = BoundaryConditions::repeat_edge(input);
        Func dxc = BoundaryConditions::repeat_edge(dx), dyc = BoundaryConditions::repeat_edge(dy);

        Line hl = filter(in, dxc, width, true, half_x, span_x, "h");
        Line vl = filter(hl.out, dyc, height, false, half_y, span_y, "v");
        output(x, y, c) = vl.out(x, y, c);

        input.dim(0).set_stride(4);
        input.dim(2).set_stride(1).set_bounds(0, 4);
        output.dim(0).set_stride(4);
        output.dim(2).set_stride(1).set_bounds(0, 4);

        // ---- schedule (as DtSharp)
        Var xo("xo"), xi("xi"), yo("yo"), yi("yi");
        const TailStrategy guard = TailStrategy::GuardWithIf;
        hl.out.compute_root().bound(c, 0, 4).reorder_storage(c, x, y);
        hl.out.split(y, yo, yi, ROWS, guard).reorder(c, x, yi, yo).vectorize(c).parallel(yo);
        hl.ct.compute_at(hl.out, yo);
        hl.area.compute_at(hl.out, yo).bound(c, 0, 4).reorder_storage(c, x, y);
        hl.area.update().reorder(c, y, hl.r).vectorize(c);
        for (DtSharp::Search &s : hl.searches) {
            s.pos.compute_at(hl.out, yi).vectorize(x, 8, guard);
            s.pos.update().reorder(x, s.k).vectorize(x, 8, guard);
        }
        output.bound(c, 0, 4).split(x, xo, xi, COLUMNS, guard).reorder(c, xi, y, xo)
            .vectorize(c).parallel(xo);
        vl.ct.compute_at(output, xo);
        vl.ct.update().reorder(x, vl.r).vectorize(x, 8, guard);
        vl.area.compute_at(output, xo).bound(c, 0, 4).reorder_storage(c, x, y);
        vl.area.update().reorder(c, x, vl.r).vectorize(c);
        for (DtSharp::Search &s : vl.searches) {
            s.pos.compute_at(output, y).vectorize(x, 8, guard);
            s.pos.update().reorder(x, s.k).vectorize(x, 8, guard);
        }
    }
};

// ---------------------------------------------------------------- GPU
//
// Sharp and Softer on the GPU: one direction of a pass per pipeline. It filters the rows of
// `input` with the distances `dist` and writes the result transposed, so that the same
// pipeline filters the columns next: a pass is
//   rows(input, dx) -> image', then rows(image', dy') -> output
// with dy' the transposed dy. The caller owns all the images: Halide allocates nothing on
// the GPU (its Metal runtime has no cache of allocations, and a new allocation is slow to
// write the first time).
//
// Each GPU block filters `tile` pixels of a row. It loads them with a margin of `margin`
// samples on each side (enough for any of their windows) into on-chip (shared) memory and
// computes running sums there (BlockSums). Each thread then searches its pixel's window
// there. On-chip memory: 20 bytes per sample, tile + 2 x margin samples (checked in
// effects/ms_edgeawareblur.c).
//
// The sums are in float (Metal has no double): over a block's few hundred samples, not over
// a whole line, and distances are cut to a length that does not change the result (each
// filter says why), so the sums stay small and precise.

// Running sums of a block's samples in on-chip memory: sums inside chunks of `chunk`
// samples, one thread per chunk, then the offset of each chunk (the sum of the chunks
// before it). pre(i, t, y[, c]): from i's chunk start to i, included. off(ch, t, y[, c]).
struct BlockSums {
    Func pre, off;
    RDom r;
    int chunk;
    bool channels;

    // `value`: sample i of block t of row y (and channel c), for i in [0, chunks x chunk).
    BlockSums(const std::string &name, Expr value, Var i, Var t, Var ch, Expr chunks, int chunk,
              bool channels)
        : pre(name + "_pre"), off(name + "_off"), r(0, chunk - 1, 0, chunks, name + "_r"),
          chunk(chunk), channels(channels) {
        Expr at = r.y * chunk + r.x;
        RDom q(0, chunks, name + "_q");
        Expr chunk_end = q * chunk + chunk - 1;
        if (channels) {
            pre(i, t, y, c) = value;
            pre(at + 1, t, y, c) += pre(at, t, y, c);
            off(ch, t, y, c) = sum(select(q < ch, pre(chunk_end, t, y, c), 0.0f));
        } else {
            pre(i, t, y) = value;
            pre(at + 1, t, y) += pre(at, t, y);
            off(ch, t, y) = sum(select(q < ch, pre(chunk_end, t, y), 0.0f));
        }
    }

    // The sum of samples 0 to j of block t, row y, included.
    Expr upto(Expr j, Expr t) const {
        return channels ? off(j / chunk, t, y, c) + pre(j, t, y, c)
                        : off(j / chunk, t, y) + pre(j, t, y);
    }

    // The sum of samples k + 1 to j (j >= k), or minus that of j + 1 to k.
    Expr diff(Expr j, Expr k, Expr t) const { return upto(j, t) - upto(k, t); }

    // Computed per block of `out` (loop `block`): loaded by all the `tile` threads, then one
    // thread per chunk, sequential inside it (chunks do not overlap: no race); channels as
    // float4. Every stage on `tile` threads, in one dimension: Halide sizes a block by the
    // largest extent of each dimension over its stages (a block over the device's limit is
    // not run, without an error), and Vulkan needs a size known at build time.
    void schedule(Func out, Var block, int tile) {
        const TailStrategy guard = TailStrategy::GuardWithIf;
        Var i = pre.args()[0], ch = off.args()[0], io("io"), ii("ii"), co("co"), ci("ci");
        RVar ro("ro"), ri("ri");
        pre.compute_at(out, block).store_in(MemoryType::GPUShared);
        pre.split(i, io, ii, tile, guard);
        pre.update().allow_race_conditions();
        off.compute_at(out, block).store_in(MemoryType::GPUShared);
        // One-channel sums can sit before the float4 ones in the block's shared memory, whose
        // float4 accesses need a multiple of 4 floats there (CUDA: channels mixed otherwise).
        if (!channels) {
            pre.align_storage(i, 4);
            off.align_storage(ch, 4);
        }
        if (channels) {
            pre.reorder_storage(c, i).reorder(c, ii, io).vectorize(c);
            pre.update().reorder(c, r.x, r.y).vectorize(c);
            off.reorder_storage(c, ch).reorder(c, ch).vectorize(c);
        } else {
            pre.reorder(ii, io);
            pre.update().reorder(r.x, r.y);
        }
        pre.gpu_threads(ii);
        pre.update().split(r.y, ro, ri, tile, guard).reorder(ri, ro).gpu_threads(ri);
        off.split(ch, co, ci, tile, guard).reorder(ci, co).gpu_threads(ci);
    }
};

// GPU Soft: the same recursions as DtSoft, in float as there, one thread per line: a
// thread per row horizontally, per column vertically (neighbouring threads read
// neighbouring columns together). The weights a^d of every pass: DtSoftWeights.
class DtSoftGpu : public Generator<DtSoftGpu> {
  public:
    Input<Buffer<float, 3>> input{"input"};
    Input<Buffer<float, 2>> wx{"wx"}, wy{"wy"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        Expr width = input.width(), height = input.height();
        Func in = BoundaryConditions::repeat_edge(input);
        Func wxc = BoundaryConditions::repeat_edge(wx), wyc = BoundaryConditions::repeat_edge(wy);

        Func fx("fx"), bx("bx"), fy("fy"), by("by");
        RDom rx(1, width - 1, "rx");
        fx(x, y, c) = in(x, y, c);
        fx(rx, y, c) = (1 - wxc(rx, y)) * in(rx, y, c) + wxc(rx, y) * fx(rx - 1, y, c);
        RDom rbx(0, width - 1, "rbx");
        Expr ix = width - 2 - rbx;
        bx(x, y, c) = fx(x, y, c);
        bx(ix, y, c) = (1 - wxc(ix + 1, y)) * fx(ix, y, c) + wxc(ix + 1, y) * bx(ix + 1, y, c);
        RDom ry(1, height - 1, "ry");
        fy(x, y, c) = bx(x, y, c);
        fy(x, ry, c) = (1 - wyc(x, ry)) * bx(x, ry, c) + wyc(x, ry) * fy(x, ry - 1, c);
        RDom rby(0, height - 1, "rby");
        Expr iy = height - 2 - rby;
        by(x, y, c) = fy(x, y, c);
        by(x, iy, c) = (1 - wyc(x, iy + 1)) * fy(x, iy, c) + wyc(x, iy + 1) * by(x, iy + 1, c);
        output(x, y, c) = by(x, y, c);

        input.dim(0).set_stride(4);
        input.dim(2).set_stride(1).set_bounds(0, 4);
        output.dim(0).set_stride(4);
        output.dim(2).set_stride(1).set_bounds(0, 4);

        // ---- schedule: each stage whole, RGBA as float4.
        Var xo("xo"), xi("xi"), yo("yo"), yi("yi");
        for (Func *f : {&fx, &bx, &fy, &by}) {
            f->compute_root().bound(c, 0, 4).reorder_storage(c, x, y);
            f->reorder(c, x, y).vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        }
        fx.update().reorder(c, rx, y).vectorize(c).gpu_tile(y, yo, yi, 64);
        bx.update().reorder(c, rbx, y).vectorize(c).gpu_tile(y, yo, yi, 64);
        fy.update().reorder(c, ry, x).vectorize(c).gpu_tile(x, xo, xi, 64);
        by.update().reorder(c, rby, x).vectorize(c).gpu_tile(x, xo, xi, 64);
        output.bound(c, 0, 4).reorder(c, x, y).vectorize(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
    }
};

// Running sums along whole rows, for windows too wide for a block's on-chip memory (the
// "rows" versions): in GPU memory, each a whole stage. In chunks of `chunk` samples: the sum
// inside the chunk up to each sample (each sample re-reads its chunk: every sample
// independent), and per row the sum of the chunks before each chunk, compensated (Kahan:
// the sum and what its rounding left out). Only differences of sums are used: the large
// sums of chunks first, so that their rounding is about the difference's size, not theirs.
struct RowSums {
    Func value, loc, before;
    RDom kk;
    int chunk;
    bool channels;

    // `v`: sample i of row y (and channel c), for i in [0, length).
    RowSums(const std::string &name, Expr v, Var i, Expr length, int chunk, bool channels)
        : value(name + "_value"), loc(name + "_loc"), before(name + "_before"),
          chunk(chunk), channels(channels) {
        RDom r(0, chunk, name + "_r");
        Expr start = i - i % chunk;
        kk = RDom(1, (length + chunk - 1) / chunk, name + "_kk");
        Expr end = kk * chunk - 1; // the last sample of chunk kk - 1
        if (channels) {
            value(i, y, c) = v;
            loc(i, y, c) = sum(select(r <= i % chunk, value(start + r, y, c), 0.0f));
            before(i, y, c) = Tuple(0.0f, 0.0f);
            Expr add = loc(end, y, c) - before(kk - 1, y, c)[1];
            Expr total = strict_float(before(kk - 1, y, c)[0] + add);
            before(kk, y, c) = Tuple(total, strict_float((total - before(kk - 1, y, c)[0]) - add));
        } else {
            value(i, y) = v;
            loc(i, y) = sum(select(r <= i % chunk, value(start + r, y), 0.0f));
            before(i, y) = Tuple(0.0f, 0.0f);
            Expr add = loc(end, y) - before(kk - 1, y)[1];
            Expr total = strict_float(before(kk - 1, y)[0] + add);
            before(kk, y) = Tuple(total, strict_float((total - before(kk - 1, y)[0]) - add));
        }
    }

    // f at sample j (part < 0: loc), or part `part` of the chunks before j's chunk.
    Expr at(Func f, Expr j, int part = -1) const {
        if (part < 0)
            return channels ? Expr(f(j, y, c)) : Expr(f(j, y));
        return channels ? Expr(f(j / chunk, y, c)[part]) : Expr(f(j / chunk, y)[part]);
    }
    Expr upto(Expr j) const { return (at(before, j, 0) - at(before, j, 1)) + at(loc, j); }
    Expr diff(Expr j, Expr k) const {
        return strict_float(((at(before, j, 0) - at(before, k, 0)) -
                             (at(before, j, 1) - at(before, k, 1))) +
                            (at(loc, j) - at(loc, k)));
    }

    // Each stage whole: the sums inside chunks one thread per sample, neighbouring threads on
    // neighbouring samples; the chunks before, one thread per row. Channels as float4.
    void schedule() {
        Var i = loc.args()[0], io("io"), ii("ii"), yo("yo"), yi("yi");
        Var bi = before.args()[0];
        loc.compute_root();
        before.compute_root();
        if (channels) {
            loc.bound(c, 0, 4).reorder_storage(c, i, y).reorder(c, i, y).vectorize(c);
            before.bound(c, 0, 4).reorder_storage(c, bi, y).reorder(c, bi, y).vectorize(c);
            before.update().reorder(c, kk.x, y).vectorize(c);
        } else {
            before.reorder(bi, y);
            before.update().reorder(kk.x, y);
        }
        loc.gpu_tile(i, y, io, yo, ii, yi, 64, 4);
        before.gpu_tile(y, yo, yi, 64);
        before.update().gpu_tile(y, yo, yi, 64);
    }
};

// The running sums of a GPU pipeline: per block (BlockSums), or per row (RowSums).
struct Sums {
    std::shared_ptr<BlockSums> block;
    std::shared_ptr<RowSums> row;
    Expr upto(Expr j, Expr t) const { return block ? block->upto(j, t) : row->upto(j); }
    Expr diff(Expr j, Expr k, Expr t) const {
        return block ? block->diff(j, k, t) : row->diff(j, k);
    }
    void schedule(Func out, Var b, int tile) {
        if (block)
            block->schedule(out, b, tile);
        else
            row->schedule();
    }
};

// What the GPU pipelines share: inputs, block layout, output schedule.
template <typename T> class DtGpu : public Generator<T> {
  public:
    // Pixels per block, samples per chunk. Small tiles keep more blocks on the GPU at once
    // but load their margins more often: the "short" versions (tile 64, chunk 8) are faster
    // for margins up to about 128 samples (CMakeLists.txt, effects/ms_edgeawareblur.c).
    GeneratorParam<int> tile{"tile", 256};
    GeneratorParam<int> chunk{"chunk", 16};
    // Running sums per row in GPU memory instead of per block on chip: any margin (the
    // "rows" versions, for windows too wide for a block).
    GeneratorParam<bool> rows{"rows", false};

  protected:
    Var i{"i"}, t{"t"}, ch{"ch"}, xo{"xo"}, xi{"xi"};

    // The layout of a row's blocks: block t holds the samples from t x tile - margin on,
    // each read at its index clamped to the row; this pixel (x on row y) is sample k of
    // block tt.
    struct Blocks {
        Expr length, margin, size, chunks, tt, k;
        Expr index(Expr t, Expr i) const { return t * tile_ - margin + i; } // in the row
        Expr pos(Expr t, Expr i) const { return clamp(index(t, i), 0, length - 1); }
        int tile_;
    };
    Blocks blocks(Expr length, Expr margin) {
        const int TILE = tile, CHUNK = chunk;
        if (rows) { // one "block": the whole row, without margin
            margin = 0;
        }
        Blocks b;
        b.tile_ = rows ? 0 : TILE;
        b.length = length;
        b.margin = margin;
        b.size = TILE + 2 * margin;            // samples per block, with its margins
        b.chunks = (b.size + CHUNK) / CHUNK;   // covers indices up to size
        b.tt = x / TILE;
        b.k = x - b.tt * TILE + margin;
        if (rows) {
            b.size = length;
            b.tt = 0;
            b.k = x;
        }
        return b;
    }

    // The block of a sample (Var t), 0 for the rows versions.
    Expr tb() const { return rows ? Expr(0) : Expr(t); }

    // The running sums of `value` (sample i of block t, or of the row).
    Sums sums_of(const std::string &name, Expr value, const Blocks &b, bool channels) {
        Sums s;
        if (rows)
            s.row = std::make_shared<RowSums>(name, value, i, b.length, (int)chunk, channels);
        else
            s.block = std::make_shared<BlockSums>(name, value, i, t, ch, b.chunks, (int)chunk,
                                                  channels);
        return s;
    }

    // output(x, y) = filtered(y, x): one block per tile of a row, one thread per pixel,
    // written to a column of the output, all 4 channels at once (float4).
    void schedule_output(Func output) {
        output.bound(c, 0, 4).split(y, xo, xi, (int)tile, TailStrategy::GuardWithIf)
            .reorder(c, xi, xo, x).vectorize(c).gpu_blocks(xo, x).gpu_threads(xi);
    }
};

// Sharp Edges on the GPU, one direction of a pass (see above): the same windows as DtSharp.
// A distance longer than radius + 2 is cut to radius + 2: such a step already leaves any
// window, so the windows do not change.
class DtSharpGpu : public DtGpu<DtSharpGpu> {
  public:
    Input<Buffer<float, 3>> input{"input"};
    Input<Buffer<float, 2>> dist{"dist"};
    Input<float> radius{"radius"}; // 0: no blur, only transposed
    Input<int> span{"span"};       // samples per side, at most
    Input<int> span_wide{"span_wide"}; // samples one more unit of distance can add, at most
    Output<Buffer<float, 3>> output{"output"}; // transposed: (y, x, c)

    void generate() {
        Func in = BoundaryConditions::repeat_edge(input);
        Func dc = BoundaryConditions::repeat_edge(dist);
        Expr length = input.width();
        Blocks bl = blocks(length, span + span_wide + 2);
        Sums d = sums_of("d", min(dc(bl.pos(tb(), i), y), radius + 2), bl, false);
        Sums v = sums_of("v", in(bl.pos(tb(), i), y, c), bl, true);

        // ct(j): position of sample j from this pixel's. values(a, b): sum of the values of
        // samples a to b.
        Expr tt = bl.tt, k = bl.k, size = bl.size;
        auto ct = [&](Expr j) { return d.diff(clamp(j, 0, size - 1), k, tt); };
        auto values = [&](Expr a, Expr b) {
            b = clamp(b, 0, size - 1);
            return select(a > 0, v.diff(b, clamp(a - 1, 0, size - 1), tt), v.upto(b, tt));
        };
        Expr eps = 1e-9f;
        Expr whole = floor(radius), fraction = radius - whole;
        Expr steps = DtSharp::log2_ceil(span + 1);
        Expr first = k - min(span, x);
        Expr last = k + min(span + 1, length - x);
        Expr end = k + (length - x); // index of position `length`
        DtSharp::Search lo = DtSharp::lower_bound(
            "lo", first, k, steps, [&](Expr j) { return ct(j) >= -whole - eps; });
        DtSharp::Search hi = DtSharp::lower_bound(
            "hi", k + 1, last, steps, [&](Expr j) { return j >= end || ct(j) > whole + eps; });
        Expr lo_edge = lo.pos(x, y);
        DtSharp::Search lo_wide = DtSharp::lower_bound(
            "low", max(lo_edge - span_wide, k - x), lo_edge, DtSharp::log2_ceil(span_wide + 1),
            [&](Expr j) { return ct(j) >= -(whole + 1) - eps; });

        Expr a = lo.pos(x, y), b = hi.pos(x, y) - 1, w = lo_wide.pos(x, y);
        Expr mean = (1.0f - fraction) / cast<float>(b - a + 1) * values(a, b);
        Expr wide = mean + fraction / cast<float>(b - w + 1) * values(w, b);
        Func filtered("filtered");
        filtered(x, y, c) = select(radius > 0, select(fraction > 0, wide, mean), in(x, y, c));
        output(x, y, c) = filtered(y, x, c);

        input.dim(0).set_stride(4);
        input.dim(2).set_stride(1).set_bounds(0, 4);
        output.dim(0).set_stride(4);
        output.dim(2).set_stride(1).set_bounds(0, 4);

        schedule_output(output);
        d.schedule(output, xo, tile);
        v.schedule(output, xo, tile);
        for (DtSharp::Search *s : {&lo, &hi, &lo_wide})
            s->pos.compute_at(output, xi);
    }
};

// Softer Edges on the GPU, one direction of a pass (see above): the same means as DtSofter.
// For the pixel at `here`, the integral over [here - h, here + h] of the interpolated
// signal is: the segments wholly inside (differences of the running areas), plus the part
// of the segment holding each end, measured from its inner end, with the segment's true
// length. Distances are cut to 2h + 2 in the running sums: a longer segment is never wholly
// inside, and the positions of the segments inside, measured from `here`, do not change.
class DtSofterGpu : public DtGpu<DtSofterGpu> {
  public:
    Input<Buffer<float, 3>> input{"input"};
    Input<Buffer<float, 2>> dist{"dist"};
    Input<float> half{"half"}; // h; 0: no blur, only transposed
    Input<int> span{"span"};   // samples per side, at most
    Output<Buffer<float, 3>> output{"output"}; // transposed: (y, x, c)

    void generate() {
        Func in = BoundaryConditions::repeat_edge(input);
        Func dc = BoundaryConditions::repeat_edge(dist);
        Expr length = input.width();
        Expr h = half;
        Blocks bl = blocks(length, span + 3);
        // Segment (j - 1, j) of the row: its length, 0 outside the row (no segment).
        Expr g = bl.index(tb(), i);
        Expr seg = select(g >= 1 && g <= length - 1, min(dc(bl.pos(tb(), i), y), 2 * h + 2), 0.0f);
        Expr trapezoid =
            seg * 0.5f * (in(bl.pos(tb(), i), y, c) + in(bl.pos(tb(), i - 1), y, c));
        Sums d = sums_of("d", seg, bl, false);
        Sums a = sums_of("a", trapezoid, bl, true);

        // Row index j <-> block index j - x + k. rel(j): position of j from here. area(j):
        // running area up to j (only differences matter).
        Expr tt = bl.tt, k = bl.k, size = bl.size;
        auto block = [&](Expr j) { return clamp(j - x + k, 0, size - 1); };
        auto rel = [&](Expr j) { return d.diff(block(j), k, tt); };
        auto area = [&](Expr j) { return a.diff(block(j), k, tt); };
        auto v = [&](Expr j) { return in(clamp(j, 0, length - 1), y, c); };
        auto len = [&](Expr j) { return dc(clamp(j, 0, length - 1), y); }; // true length

        // First index (from 1 on) whose position is past t = here + offset; the segment
        // holding t starts one before it (as DtSofter).
        Expr steps = DtSharp::log2_ceil(span + 2);
        auto above = [&](Expr offset) {
            return [=](Expr j) { return j >= length - 1 || rel(j) > offset; };
        };
        DtSharp::Search low = DtSharp::lower_bound("low", max(x - span, 1),
                                                   max(min(x + 1, length - 1), 1), steps, above(-h));
        DtSharp::Search high = DtSharp::lower_bound("high", max(min(x, length - 1), 1),
                                                    max(min(x + span + 1, length - 1), 1), steps,
                                                    above(h));
        Expr kl = low.pos(x, y) - 1, kh = high.pos(x, y) - 1;

        // From here - h to here: before the row's start (constant), or the end of segment
        // (kl, kl + 1) from its inner end, w long.
        Expr w = rel(kl + 1) + h;
        Expr lower = select(rel(0) >= -h, -area(0) + (rel(0) + h) * v(0),
                            -area(kl + 1) + w * v(kl + 1) -
                                0.5f * w * w * (v(kl + 1) - v(kl)) / len(kl + 1));
        // From here to here + h: past the row's end (constant), or the start of segment
        // (kh, kh + 1), u long.
        Expr u = h - rel(kh);
        Expr upper = select(rel(length - 1) <= h,
                            area(length - 1) + (h - rel(length - 1)) * v(length - 1),
                            area(kh) + u * v(kh) + 0.5f * u * u * (v(kh + 1) - v(kh)) / len(kh + 1));
        Expr mean = (lower + upper) / (2 * h);
        Func filtered("filtered");
        filtered(x, y, c) = select(h > 0 && length > 1, mean, in(x, y, c));
        output(x, y, c) = filtered(y, x, c);

        input.dim(0).set_stride(4);
        input.dim(2).set_stride(1).set_bounds(0, 4);
        output.dim(0).set_stride(4);
        output.dim(2).set_stride(1).set_bounds(0, 4);

        schedule_output(output);
        d.schedule(output, xo, tile);
        a.schedule(output, xo, tile);
        for (DtSharp::Search *s : {&low, &high})
            s->pos.compute_at(output, xi);
    }
};

// A one-channel image transposed: (x, y) -> (y, x). For the GPU Sharp's and Softer's
// distances along y (see DtGpu).
class DtTranspose : public Generator<DtTranspose> {
  public:
    Input<Buffer<float, 2>> input{"input"};
    Output<Buffer<float, 2>> output{"output"};

    void generate() {
        output(x, y) = input(y, x);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        if (get_target().has_gpu_feature())
            output.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            output.tile(x, y, xo, yo, xi, yi, 16, 16).vectorize(xi, 8).parallel(yo);
    }
};

HALIDE_REGISTER_GENERATOR(DtSharp, ms_dt_sharp)
HALIDE_REGISTER_GENERATOR(DtSharpGpu, ms_dt_sharp_gpu)
HALIDE_REGISTER_GENERATOR(DtTranspose, ms_dt_transpose)
HALIDE_REGISTER_GENERATOR(DtSofter, ms_dt_softer)
HALIDE_REGISTER_GENERATOR(DtSofterGpu, ms_dt_softer_gpu)
HALIDE_REGISTER_GENERATOR(DtDistances, ms_dt_distances)
HALIDE_REGISTER_GENERATOR(DtSoftWeights, ms_dt_soft_weights)
HALIDE_REGISTER_GENERATOR(DtSoft, ms_dt_soft)
HALIDE_REGISTER_GENERATOR(DtSoftGpu, ms_dt_soft_gpu)
HALIDE_REGISTER_GENERATOR(DtEdgeSmooth, ms_dt_edge_smooth)
HALIDE_REGISTER_GENERATOR(DtLargest, ms_dt_largest)
