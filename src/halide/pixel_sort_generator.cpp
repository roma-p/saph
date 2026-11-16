// MS_PixelSort in Halide, called by effects/ms_pixelsort.c. The image (or its area mean at the
// Sort Resolution: PixelSortDown) is cut into lines whose spans are sorted:
//   - the lines: Linear, digital lines along the Sort Angle (one pixel along the major axis at
//     a time, the other coordinate floor(slope x + 0.5) off: every pixel on one line); Radial,
//     rays from the Center (as many as pixels on the outer circle), samples one pixel apart
//     outwards, each ray's start moved out by a random share of Vary Radius x the distance to
//     the image's farthest corner (its end stays at Inner Radius + Ray Length);
//     Circular, rings one pixel wide around the Center, samples one pixel apart
//     counter-clockwise. Rays and rings sample the image (the nearest pixel), and each pixel
//     reads back the sample it falls on
//   - the sort value of the pixels (Sort Type); the threshold mask: the value at or above the
//     Threshold (Sort Direction: below it), softened by 3 box blurs (edges clamped), times the
//     Mask (post-threshold)
//   - the spans: runs of samples whose mask is above their line's own random threshold
//     (uniform 0..1: with a mask of 0 and 1, the mask itself), split by the restart pixels
//     (a random share of the pixels, Randomly Restart Sort) and by the line's last sample
//     (last 2 off the axes), which never move
//   - each line sorted whole on (its span's first sample, value, sample), a bitonic network
//     (PixelSortStep: one compare-and-swap pass per call; PixelSortBlock: the passes at short
//     distances, block by block): the spans stay where they are,
//     sorted ascending (Reverse: descending)
//   - the result (PixelSortOut): each pixel takes what the sort brought to its sample, back to
//     full resolution (a pixel its cell; the pixel a cell's end falls in, the next cell by
//     the part of the cell past that end), shown (Show), mixed by the Mask (pre-effect), then
//     with the Source (Mix With Source)
//
// The lines' buffer: (2 P, lines) uint32, sample i at 2 i (its value as ordered bits) and
// 2 i + 1 (its span's first sample << 16 | i), P samples per line (a power of 2). Images are
// packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "noise.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c"), i("i"), l("l");

using ms_noise::u32;

const float TAU = 6.28318530718f;

// 0..1 from a 32-bit hash.
Expr uniform(Expr h) { return cast<float>(h >> u32(8)) * (1.0f / 16777216.0f); }

// The sort value (Sort Type) of a pixel.
Expr sort_value(Expr type, Expr r, Expr g, Expr b) {
    Expr mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
    Expr safe = select(d > 0.0f, d, 1.0f);
    Expr h6 = select(mx == r, (g - b) / safe, mx == g, (b - r) / safe + 2.0f,
                     (r - g) / safe + 4.0f);
    Expr hue = select(d > 0.0f, (h6 - 6.0f * floor(h6 / 6.0f)) / 6.0f, 0.0f);
    Expr saturation = select(mx > 0.0f, d / mx, 0.0f);
    // (strict: the same on the CPU and the GPU, no fused multiply-adds, for the same order)
    return strict_float(select(type == 0, 0.3125f * r + 0.5f * g + 0.1875f * b, type == 1,
                               (r + g + b) / 3.0f, type == 2, mn, type == 3, mx, type == 4, r,
                               type == 5, g, type == 6, b, type == 7, hue, type == 8, saturation,
                               mx));
}

// The restart pixels of the sorted image.
Expr restarts(Expr px, Expr py, Expr seed, Expr share) {
    return uniform(ms_noise::hash3(px, py, seed + 7919)) < share;
}

Expr wrap(Expr angle) { return angle - TAU * floor(angle / TAU); }

// The lines, in pixels of the sorted image (x right, y up).
struct Lines {
    Expr mode;                    // 0 linear, 1 radial, 2 circular
    Expr major, dir, slope, offset; // linear: x / y major, its direction, the slope across
    Expr cx, cy, inner, extent;   // the centre; the first radius, the rays' length / rings'
    Expr vary, a0, span, step;    // radial: the rays' largest move out (pixels), circular:
                                  // Vary Start; the start angle and the angles sorted
                                  // (radians); radial: the angle between two rays
    Expr seed;
    Expr width, height, samples, count; // the sorted image's size; P, the number of lines

    Expr random(Expr line, int salt) const { return uniform(ms_noise::hash3(line, salt, seed)); }
    // The threshold of the spans of a line.
    Expr threshold(Expr line) const { return random(line, 0); }
    // The first radius of a ray.
    Expr ray_start(Expr line) const { return inner + vary * random(line, 1); }
    Expr ring_offset(Expr line) const { return vary * random(line, 1) * TAU; }
    Expr ring_radius(Expr line) const { return inner + cast<float>(line) + 0.5f; }
    // Linear: the line's offset across at `along` (strict: the same lines on the CPU and the
    // GPU).
    Expr across_offset(Expr along) const {
        return cast<int>(floor(strict_float(slope * cast<float>(along) + 0.5f)));
    }

    // The pixel (px, py) of sample s of a line, and whether it has one.
    void pixel(Expr line, Expr s, Expr &px, Expr &py, Expr &valid) const {
        // linear
        Expr n = select(major == 0, width, height), n_across = select(major == 0, height, width);
        Expr along = select(dir > 0, s, n - 1 - s);
        Expr across = line - offset + across_offset(along);
        // radial and circular
        Expr fs = cast<float>(s) + 0.5f, rr = ring_radius(line);
        Expr angle = select(mode == 1, a0 + (cast<float>(line) + 0.5f) * step,
                            a0 + ring_offset(line) + fs / rr);
        Expr radius = select(mode == 1, ray_start(line) + fs, rr);
        Expr qx = cast<int>(floor(cx + radius * cos(angle)));
        Expr qy = cast<int>(floor(cy + radius * sin(angle)));
        Expr inside = select(mode == 1, radius < inner + extent, fs / rr < span) && qx >= 0 &&
                      qx < width && qy >= 0 && qy < height;
        px = select(mode == 0, select(major == 0, along, across), qx);
        py = select(mode == 0, select(major == 0, across, along), qy);
        valid = s >= 0 && s < samples &&
                select(mode == 0, s < n && across >= 0 && across < n_across, inside);
    }

    // The line and sample of pixel (px, py), and whether it is on one.
    void sample(Expr px, Expr py, Expr &line, Expr &s, Expr &on) const {
        // linear
        Expr n = select(major == 0, width, height);
        Expr along = select(major == 0, px, py), across = select(major == 0, py, px);
        Expr ls = select(dir > 0, along, n - 1 - along);
        Expr ll = across - across_offset(along) + offset;
        // radial and circular
        Expr dx = cast<float>(px) + 0.5f - cx, dy = cast<float>(py) + 0.5f - cy;
        Expr r = sqrt(dx * dx + dy * dy), theta = atan2(dy, dx);
        Expr ray = clamp(cast<int>(floor(wrap(theta - a0) / step)), 0, count - 1);
        Expr ring = cast<int>(floor(r - inner));
        Expr ring_rel = wrap(theta - a0 - ring_offset(ring));
        Expr polar_line = select(mode == 1, ray, ring);
        Expr out = r - ray_start(ray);
        Expr polar_s = select(mode == 1, cast<int>(floor(out)),
                              cast<int>(floor(ring_rel * ring_radius(ring))));
        Expr polar_on = select(mode == 1, wrap(theta - a0) < span && out >= 0.0f && r < inner + extent,
                               r >= inner && ring_rel < span);
        line = select(mode == 0, ll, polar_line);
        s = select(mode == 0, ls, polar_s);
        on = select(mode == 0, true, polar_on) && line >= 0 && line < count && s >= 0 &&
             s < samples;
    }
};

// The images of the mask and the lines (a generator's inputs).
#define LINES_INPUTS                                                                               \
    Input<int> mode{"mode"}, major{"major"}, dir{"dir"};                                           \
    Input<float> slope{"slope"};                                                                   \
    Input<int> offset{"offset"};                                                                   \
    Input<float> cx{"cx"}, cy{"cy"}, inner{"inner"}, extent{"extent"}, vary{"vary"}, a0{"a0"},    \
        span{"span"}, step{"step"};                                                                \
    Input<int> seed{"seed"};                                                                       \
    Input<float> restart{"restart"};                                                               \
    Input<int> sort_type{"sort_type"}, mask_post{"mask_post"};

// The image to sort's lines: their values and spans.
class PixelSortPrep : public Generator<PixelSortPrep> {
public:
    Input<Buffer<float, 3>> image{"image"};           // the image sorted
    Input<Buffer<float, 2>> mask_value{"mask_value"}; // ms_mask, full size; none: 1 (strides 0)
    LINES_INPUTS
    Input<float> threshold{"threshold"};
    Input<int> above{"above"}, reverse{"reverse"}, ends{"ends"};
    Input<int> soften1{"soften1"}, soften2{"soften2"}, soften3{"soften3"}; // box radii
    Input<int> samples{"samples"}, count{"count"};
    Output<Buffer<uint32_t, 2>> lines{"lines"};
    Output<Buffer<float, 2>> soft{"soft"}; // the softened threshold mask

    void generate() {
        Expr w = image.dim(0).extent(), h = image.dim(1).extent();
        Lines g{mode, major, dir, slope, offset, cx, cy, inner, extent, vary, a0, span, step,
                seed, w, h, samples, count};
        Func in = BoundaryConditions::repeat_edge(image);
        auto value = [&](Expr px, Expr py) {
            return sort_value(sort_type, in(px, py, 0), in(px, py, 1), in(px, py, 2));
        };
        Func binary("binary");
        Expr v = value(x, y);
        binary(x, y) = select(select(above != 0, v >= threshold, v < threshold), 1.0f, 0.0f);
        // the 3 boxes along x, then along y
        std::vector<Func> boxes;
        Func last = binary;
        Expr radii[3] = {soften1, soften2, soften3};
        for (int k = 0; k < 6; ++k) {
            const bool along_x = k < 3;
            Expr r = radii[k % 3];
            Func edged = BoundaryConditions::repeat_edge(last, {{0, w}, {0, h}});
            RDom t(0, 2 * r + 1, "t" + std::to_string(k));
            Func box("box" + std::to_string(k));
            box(x, y) = sum(along_x ? edged(x + t - r, y) : edged(x, y + t - r)) /
                        cast<float>(2 * r + 1);
            boxes.push_back(box);
            last = box;
        }
        soft(x, y) = last(x, y);

        // A sample in a span: on the line, its mask above the line's threshold, not a restart
        // pixel, not the line's last (last 2 off the axes).
        Func soft_edged = BoundaryConditions::repeat_edge(soft, {{0, w}, {0, h}});
        Expr mw = mask_value.dim(0).extent(), mh = mask_value.dim(1).extent();
        Func spans("spans");
        {
            Expr px, py, valid, px1, py1, valid1, px2, py2, valid2;
            g.pixel(l, i, px, py, valid);
            g.pixel(l, i + 1, px1, py1, valid1);
            g.pixel(l, i + 2, px2, py2, valid2);
            Expr mx = clamp(cast<int>(floor((cast<float>(px) + 0.5f) * cast<float>(mw) / cast<float>(w))), 0, mw - 1);
            Expr my = clamp(cast<int>(floor((cast<float>(py) + 0.5f) * cast<float>(mh) / cast<float>(h))), 0, mh - 1);
            Expr m = soft_edged(px, py) * select(mask_post != 0, mask_value(mx, my), 1.0f);
            Expr end = !valid1 || (ends > 1 && !valid2);
            spans(i, l) = cast<uint8_t>(valid && m > g.threshold(l) &&
                                        !restarts(px, py, seed, restart) && !end);
        }
        // Each sample's span's first sample (a sample out of the spans: its own).
        Func first("first");
        RDom s(1, samples - 1, "s");
        first(i, l) = i;
        first(s, l) = select(spans(s, l) != 0 && spans(s - 1, l) != 0, first(s - 1, l), s);

        Expr px, py, valid;
        g.pixel(l, x / 2, px, py, valid);
        Expr bits = reinterpret<uint32_t>(select(valid, value(px, py), 0.0f));
        bits = select((bits >> u32(31)) != u32(0), ~bits, bits | u32(0x80000000u));
        bits = select(reverse != 0, ~bits, bits);
        lines(x, l) = select(x % 2 == 0, bits,
                             (cast<uint32_t>(first(x / 2, l)) << u32(16)) | cast<uint32_t>(x / 2));

        ms::interleaved(image);
        mask_value.dim(0).set_stride(Expr()); // without a mask: one value (stride 0)
        if (get_target().has_gpu_feature()) {
            Var xo, yo, xi, yi;
            for (Func &b : boxes)
                b.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            soft.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            spans.compute_root().gpu_tile(i, l, xo, yo, xi, yi, 64, 4);
            first.compute_root().gpu_tile(i, l, xo, yo, xi, yi, 64, 4);
            first.update().gpu_tile(l, yo, yi, 64);
            lines.gpu_tile(x, l, xo, yo, xi, yi, 64, 4);
        } else {
            for (Func &b : boxes)
                b.compute_root().vectorize(x, 8).parallel(y);
            soft.vectorize(x, 8).parallel(y);
            spans.compute_root().vectorize(i, 16).parallel(l);
            first.compute_root().vectorize(i, 8).parallel(l);
            first.update().parallel(l);
            lines.vectorize(x, 8).parallel(l);
        }
    }
};

// The bitonic network's compare-and-swap of sample s (value v, tag t) with its partner p
// (bv, bt), in blocks of k samples alternately ascending and descending: (value, tag) of the
// one s keeps.
Tuple compare_swap(Expr s, Expr p, Expr k, Expr v, Expr t, Expr bv, Expr bt) {
    Expr first = t >> u32(16), bfirst = bt >> u32(16);
    Expr b_less = bfirst < first ||
                  (bfirst == first && (bv < v || (bv == v && (bt & u32(0xffff)) < (t & u32(0xffff)))));
    Expr ascending = (s & k) == 0;
    Expr take_b = select((s < p) == ascending, b_less, !b_less);
    return Tuple(select(take_b, bv, v), select(take_b, bt, t));
}

// One pass of the bitonic network over every line: sample s against s ^ j.
class PixelSortStep : public Generator<PixelSortStep> {
public:
    Input<Buffer<uint32_t, 2>> before{"before"};
    Input<int> k{"k"}, j{"j"};
    Output<Buffer<uint32_t, 2>> after{"after"};

    void generate() {
        Expr s = x / 2, p = clamp(s ^ j, 0, before.dim(0).extent() / 2 - 1); // (always in)
        Tuple kept = compare_swap(s, p, k, before(2 * s, l), before(2 * s + 1, l),
                                  before(2 * p, l), before(2 * p + 1, l));
        after(x, l) = select(x % 2 == 0, kept[0], kept[1]);
        if (get_target().has_gpu_feature()) {
            Var xo, yo, xi, yi;
            after.gpu_tile(x, l, xo, yo, xi, yi, 128, 2);
        } else {
            after.vectorize(x, 8).parallel(l);
        }
    }
};

// The passes of one merge at distances j, j / 2 .. 1, all under BLOCK (blocks of BLOCK
// samples, each in on-chip memory or the cache; ms_pixelsort.c's PIXEL_SORT_BLOCK).
const int BLOCK = 512, BLOCK_PASSES = 9; // (log2)

class PixelSortBlock : public Generator<PixelSortBlock> {
public:
    Input<Buffer<uint32_t, 2>> before{"before"};
    Input<int> k{"k"}, j{"j"}; // j < BLOCK
    Output<Buffer<uint32_t, 2>> after{"after"};

    void generate() {
        // (w, block, line): word w of the block's 2 BLOCK
        Var w("w"), b("b");
        Func pass[BLOCK_PASSES + 1];
        pass[0] = Func("pass0");
        // (lines shorter than a block: past their end, never read by their samples)
        pass[0](w, b, l) = before(min(b * (2 * BLOCK) + w, before.dim(0).extent() - 1), l);
        for (int n = 1; n <= BLOCK_PASSES; ++n) {
            // pass n at distance j >> (n - 1): none past distance 1
            Expr d = j >> (n - 1);
            Expr s = w / 2, p = clamp(s ^ d, 0, BLOCK - 1);
            Func &in = pass[n - 1];
            Tuple kept = compare_swap(b * BLOCK + s, b * BLOCK + p, k, in(2 * s, b, l),
                                      in(2 * s + 1, b, l), in(2 * p, b, l), in(2 * p + 1, b, l));
            pass[n] = Func("pass" + std::to_string(n));
            pass[n](w, b, l) = select(d == 0, in(w, b, l), select(w % 2 == 0, kept[0], kept[1]));
        }
        Var xo("xo"), xi("xi");
        after(x, l) = pass[BLOCK_PASSES](x % (2 * BLOCK), x / (2 * BLOCK), l);
        after.split(x, xo, xi, 2 * BLOCK, TailStrategy::GuardWithIf);
        if (get_target().has_gpu_feature()) {
            after.gpu_blocks(xo, l).gpu_threads(xi);
            for (int n = 0; n < BLOCK_PASSES; ++n)
                pass[n].compute_at(after, xo).bound(w, 0, 2 * BLOCK).gpu_threads(w);
        } else {
            after.vectorize(xi, 8).parallel(l);
            for (int n = 0; n < BLOCK_PASSES; ++n)
                pass[n].compute_at(after, xo).bound(w, 0, 2 * BLOCK).vectorize(w, 8);
        }
    }
};

// The image at the Sort Resolution: each cell (from the bottom-left) the mean of the pixels
// it covers, by the part of each it covers.
class PixelSortDown : public Generator<PixelSortDown> {
public:
    Input<Buffer<float, 3>> source{"source"};
    Input<int> reach_x{"reach_x"}, reach_y{"reach_y"}; // pixels a cell touches, at most
    Output<Buffer<float, 3>> image{"image"};

    void generate() {
        Expr w = source.dim(0).extent(), h = source.dim(1).extent();
        Expr sw = image.dim(0).extent(), sh = image.dim(1).extent();
        Expr cw = cast<float>(w) / cast<float>(sw), ch = cast<float>(h) / cast<float>(sh);
        Func in = BoundaryConditions::repeat_edge(source);
        RDom kx(0, reach_x, "kx"), ky(0, reach_y, "ky");
        auto cover = [](Expr p, Expr lo, Expr hi) {
            Expr fp = cast<float>(p);
            return max(0.0f, min(fp + 1.0f, hi) - max(fp, lo));
        };
        Func across("across");
        Expr x0 = cast<float>(x) * cw, px = cast<int>(floor(x0)) + kx;
        across(x, y, c) = sum(in(px, y, c) * cover(px, x0, x0 + cw)) / cw;
        Expr y0 = cast<float>(y) * ch, py = cast<int>(floor(y0)) + ky;
        Func across_edged = BoundaryConditions::repeat_edge(across, {{0, sw}, {0, h}, {0, 4}});
        image(x, y, c) = sum(across_edged(x, py, c) * cover(py, y0, y0 + ch)) / ch;

        ms::interleaved(source);
        ms::interleaved(image);
        image.bound(c, 0, 4);
        if (get_target().has_gpu_feature()) {
            Var xo, yo, xi, yi;
            across.compute_root().reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            image.reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        } else {
            across.compute_root().reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
            image.reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
        }
    }
};

// The sorted lines back on the image, shown, masked and mixed.
class PixelSortOut : public Generator<PixelSortOut> {
public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 3>> image{"image"}; // the image sorted (the Source at full resolution)
    Input<Buffer<float, 2>> soft{"soft"};
    Input<Buffer<uint32_t, 2>> lines{"lines"}; // sorted
    Input<Buffer<float, 2>> mask_value{"mask_value"};
    LINES_INPUTS
    Input<int> show{"show"}, mask_pre{"mask_pre"};
    Input<float> mix{"mix"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        Expr w = source.dim(0).extent(), h = source.dim(1).extent();
        Expr sw = image.dim(0).extent(), sh = image.dim(1).extent();
        Expr samples = lines.dim(0).extent() / 2, count = lines.dim(1).extent();
        Lines g{mode, major, dir, slope, offset, cx, cy, inner, extent, vary, a0, span, step,
                seed, sw, sh, samples, count};
        Func in = BoundaryConditions::repeat_edge(image);

        // At the Sort Resolution: what the sort brought to each pixel, or what Show shows.
        Func shown("shown");
        {
            Expr line, s, on, qx, qy, valid;
            g.sample(x, y, line, s, on);
            Expr tag = lines(clamp(2 * s + 1, 0, 2 * samples - 1), clamp(line, 0, count - 1));
            Expr from = cast<int>(tag & u32(0xffff));
            g.pixel(line, from, qx, qy, valid);
            Expr moved = on && from != s && valid;
            Expr sorted = select(moved, in(qx, qy, c), in(x, y, c));
            Expr m = soft(x, y) * select(mask_post != 0, mask_value(clamp(cast<int>(floor((cast<float>(x) + 0.5f) * cast<float>(w) / cast<float>(sw))), 0, w - 1), clamp(cast<int>(floor((cast<float>(y) + 0.5f) * cast<float>(h) / cast<float>(sh))), 0, h - 1)), 1.0f);
            Expr restarted = restarts(x, y, seed, restart);
            Expr combined = select(on && m > g.threshold(line) && !restarted, 1.0f, 0.0f);
            Expr grey = select(show == 1, sort_value(sort_type, in(x, y, 0), in(x, y, 1), in(x, y, 2)),
                               show == 2, soft(x, y), show == 3, select(restarted, 1.0f, 0.0f),
                               combined);
            shown(x, y, c) = select(show == 0, sorted, c == 3, 1.0f, grey);
        }

        // Back to full resolution: each pixel its cell, or the next one by the part of the
        // pixel past the cell's end.
        auto cell = [](Expr p, Expr n, Expr sn, Expr &a, Expr &next, Expr &t) {
            a = clamp(p * sn / n, 0, sn - 1);
            Expr end = cast<float>(a + 1) * cast<float>(n) / cast<float>(sn);
            t = clamp((cast<float>(p) + 1.0f - end) * cast<float>(sn) / cast<float>(n), 0.0f, 1.0f);
            next = min(a + 1, sn - 1);
        };
        Expr ax, nx, tx, ay, ny, ty;
        cell(x, w, sw, ax, nx, tx);
        cell(y, h, sh, ay, ny, ty);
        Expr up = (1.0f - ty) * ((1.0f - tx) * shown(ax, ay, c) + tx * shown(nx, ay, c)) +
                  ty * ((1.0f - tx) * shown(ax, ny, c) + tx * shown(nx, ny, c));
        Expr src = source(x, y, c);
        Expr masked = select(mask_pre != 0, src + (up - src) * mask_value(x, y), up);
        output(x, y, c) = select(show == 0, masked + (src - masked) * mix, up);

        ms::interleaved(source);
        ms::interleaved(image);
        ms::interleaved(output);
        mask_value.dim(0).set_stride(Expr());
        output.bound(c, 0, 4);
        shown.bound(c, 0, 4);
        if (get_target().has_gpu_feature()) {
            Var xo, yo, xi, yi;
            shown.compute_root().reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            output.reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        } else {
            shown.compute_root().reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
            output.reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
        }
    }
};

} // namespace

HALIDE_REGISTER_GENERATOR(PixelSortPrep, ms_pixel_sort_prep)
HALIDE_REGISTER_GENERATOR(PixelSortStep, ms_pixel_sort_step)
HALIDE_REGISTER_GENERATOR(PixelSortBlock, ms_pixel_sort_block)
HALIDE_REGISTER_GENERATOR(PixelSortDown, ms_pixel_sort_down)
HALIDE_REGISTER_GENERATOR(PixelSortOut, ms_pixel_sort_out)
