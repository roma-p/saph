// The 3-box blur of the effects: 3 boxes along x, then 3 along y, borders mirrored (pixel -1
// = pixel 0, repeated for a box wider than the image). The total radius (rounded by the
// caller) is split into 3 near-equal box radii.

#pragma once

#include "Halide.h"

namespace ms_blur {

using namespace Halide;

// One box: the mean over [i - r, i + r] along x or y.
struct Box {
    Func out;
    Func running; // CPU: running sum along the line; GPU sliding: along a run of pixels
    Func first;   // GPU sliding: the window of the first pixel of each run
    bool along_x;
    bool sliding;
    Expr radius;
    Func input; // GPU windows: the mirrored input
    bool prefix = false;
    Func chunks; // GPU prefix: the sum of each chunk of the line
    Func before; // GPU prefix: the sum of the chunks before each chunk
    RDom scan;   // GPU prefix: the chunks of before's update
};

// GPU sliding boxes: each thread slides the window along a run of SLIDE pixels (adds the
// pixel entering it, subtracts the one leaving it): about 2 + (2r + 1) / SLIDE reads per
// pixel instead of 2r + 1, for large radii.
constexpr int SLIDE = 32;

// GPU prefix boxes: each window is the difference of two running sums of the line, from the
// mirrored pixel -r - 1 on: the same cost whatever the radius. A running sum is the sum of the
// chunks (CHUNK pixels) before the pixel's chunk, kept per line, plus the pixels of its chunk
// up to it, read again: every pixel independent, as the windows. In float (Metal has no
// double): the line's first pixel is taken off every pixel first, to keep the sums small.
constexpr int CHUNK = 16;

inline Box box(Func in, Expr r, bool along_x, Expr width, Expr height, bool gpu, bool sliding,
               const std::string &name, bool prefix = false) {
    Var x = in.args()[0], y = in.args()[1], c = in.args()[2];
    Func mirrored = BoundaryConditions::mirror_image(in, {{0, width}, {0, height}});
    Box b{Func(name), Func(name + "_sum"), Func(name + "_first"), along_x, gpu && sliding, r};
    Expr n = cast<float>(2 * r + 1);
    if (gpu && prefix) {
        b.prefix = true;
        b.chunks = Func(name + "_chunks");
        b.before = Func(name + "_before");
        // Along the line: the first coordinate; across it: o. The line starts at pixel -r - 1.
        Var k("k"), o("o");
        Expr start = -r - 1, length = along_x ? width : height;
        auto at = [&](Expr along, Expr other) {
            return along_x ? mirrored(along, other, c) : mirrored(other, along, c);
        };
        auto v = [&](Expr along, Expr other) { return at(along, other) - at(0, other); };
        RDom j(0, CHUNK, name + "_j");
        b.chunks(k, o, c) = sum(v(start + k * CHUNK + j, o));
        // Chunks of the line: up to the last one a window of the image reaches.
        RDom kk(1, (length + 2 * r + 1) / CHUNK + 1, name + "_kk");
        // Compensated (Kahan): the sum, and what its rounding left out (strict: kept as is).
        b.before(k, o, c) = Tuple(0.0f, 0.0f);
        Expr add = b.chunks(kk - 1, o, c) - b.before(kk - 1, o, c)[1];
        Expr total = strict_float(b.before(kk - 1, o, c)[0] + add);
        b.before(kk, o, c) = Tuple(total, strict_float((total - b.before(kk - 1, o, c)[0]) - add));
        b.scan = kk;
        // The running sum up to pixel `along`, included.
        // The running sum up to pixel `along`, included: the sum of the chunks before its
        // chunk (with what its rounding left out), and its chunk up to it.
        struct Sum {
            Expr chunks, left_out, inside;
        };
        auto sums = [&](Expr along, Expr other) {
            Expr from = along - start, chunk = from / CHUNK;
            RDom i(0, CHUNK, name + "_i");
            return Sum{b.before(chunk, other, c)[0], b.before(chunk, other, c)[1],
                       sum(select(i <= from % CHUNK, v(start + chunk * CHUNK + i, other), 0.0f))};
        };
        Expr along = along_x ? Expr(x) : Expr(y), other = along_x ? Expr(y) : Expr(x);
        Sum s2 = sums(along + r, other), s1 = sums(along - r - 1, other);
        // The large sums of chunks first: their difference is about the window's size, so
        // its rounding is too (not the sums').
        Expr window = strict_float(((s2.chunks - s1.chunks) - (s2.left_out - s1.left_out)) +
                                   (s2.inside - s1.inside));
        b.out(x, y, c) = select(r == 0, mirrored(x, y, c), window / n + at(0, other));
    } else if (gpu && sliding) {
        // run(i, run index, other coordinate, c): window sum of pixel run x SLIDE + i.
        Var i("i"), run("run"), o("o");
        RDom k(-r, 2 * r + 1, name + "_k");
        RDom j(1, SLIDE - 1, name + "_j");
        auto at = [&](Expr along, Expr other) {
            return along_x ? mirrored(along, other, c) : mirrored(other, along, c);
        };
        b.first(run, o, c) = sum(at(run * SLIDE + k, o));
        b.running(i, run, o, c) = b.first(run, o, c);
        b.running(j, run, o, c) = b.running(j - 1, run, o, c) + at(run * SLIDE + j + r, o) -
                                  at(run * SLIDE + j - 1 - r, o);
        Expr u = along_x ? Expr(x) : Expr(y), other = along_x ? Expr(y) : Expr(x);
        // Radius 0: the input itself (sliding adds and subtracts pixels: in float, not
        // exactly the pixel, and the Distort shift magnifies any difference of its lens).
        // Read mirrored: Halide's bounds for these boxes can reach past the image.
        b.out(x, y, c) =
            select(r == 0, mirrored(x, y, c), b.running(u % SLIDE, u / SLIDE, other, c) / n);
    } else if (gpu) {
        // GPU: every pixel sums its own window: fully parallel. (A running sum per piece
        // of line was tried: barely faster here, less precise, as Metal has no double.)
        RDom k(0, 2 * r + 1, name + "_k");
        Expr xs = along_x ? x + k - r : Expr(x);
        Expr ys = along_x ? Expr(y) : y + k - r;
        b.first(x, y, c) = 0.0f; // the window sum
        b.first(x, y, c) += mirrored(xs, ys, c);
        b.out(x, y, c) = b.first(x, y, c) / n;
        b.input = mirrored;
    } else {
        // CPU: a running sum along the line (in double), then each
        // window is the difference of two sums: the same cost whatever the radius.
        Expr length = along_x ? width : height;
        RDom k(-r - 1, length + 2 * r + 1, name + "_k");
        Func &s = b.running;
        s(x, y, c) = cast<double>(0);
        if (along_x)
            s(k, y, c) = s(k - 1, y, c) + cast<double>(mirrored(k, y, c));
        else
            s(x, k, c) = s(x, k - 1, c) + cast<double>(mirrored(x, k, c));
        Expr window =
            along_x ? s(x + r, y, c) - s(x - r - 1, y, c) : s(x, y + r, c) - s(x, y - r - 1, c);
        // Radius 0 (e.g. no blur at all): the input itself, without the running sum (see
        // schedule()).
        b.out(x, y, c) = select(r == 0, mirrored(x, y, c), cast<float>(window) / n);
    }
    return b;
}

// The 3 box radii of a blur of total radius `total`.
inline Expr box_radius(Expr total, int i) { return total / 3 + select(i >= 3 - total % 3, 1, 0); }

// The 3-box blur of `in` (x, y, c): 3 horizontal boxes, then 3 vertical ones. Appends its 6
// boxes to `boxes` (for schedule()).
inline Func blur3(Func in, Expr total_x, Expr total_y, Expr width, Expr height, bool gpu,
                  const std::string &name, std::vector<Box> &boxes, bool sliding = false,
                  bool prefix = false) {
    Func f = in;
    for (int i = 0; i < 6; ++i) {
        const bool along_x = i < 3;
        Expr r = box_radius(along_x ? total_x : total_y, i % 3);
        boxes.push_back(
            box(f, r, along_x, width, height, gpu, sliding, name + std::to_string(i), prefix));
        f = boxes.back().out;
    }
    return f;
}

// GPU RGB boxes with `staged`: each block first loads its pixels and their windows into
// on-chip memory, then sums the windows there. Blocks of 64 x 4 pixels along x, 8 x 32
// along y: on-chip memory (64 + 2r) x 4 or 8 x (32 + 2r) float4, within 32 KB (Metal) for
// box radii up to STAGED_RADIUS_X / _Y (the caller checks).
constexpr int STAGED_RADIUS_X = 200, STAGED_RADIUS_Y = 100;

// Each box computed whole, over `channels` channels (1, or 3 / 4 for RGB(A)).
inline void schedule(std::vector<Box> &boxes, bool gpu, int channels, bool staged = false) {
    for (Box &b : boxes) {
        Var x = b.out.args()[0], y = b.out.args()[1], c = b.out.args()[2];
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        const int lanes = channels > 1 ? 4 : 1; // RGB as float4 (a fourth channel unused)
        if (b.prefix) {
            // The chunks: one thread each; before: one thread per line; the windows: as the
            // other GPU boxes, RGB as float4.
            auto rgb = [&](Func f) {
                Var fc = f.args()[2];
                f.bound(fc, 0, lanes).reorder_storage(fc, f.args()[0], f.args()[1]);
                if (channels > 1)
                    f.vectorize(fc);
            };
            Var k = b.chunks.args()[0], o = b.chunks.args()[1], kc = b.chunks.args()[2];
            Var ko("ko"), oo("oo"), ki("ki"), oi("oi");
            b.chunks.compute_root().reorder(kc, k, o);
            if (b.along_x)
                b.chunks.gpu_tile(k, o, ko, oo, ki, oi, 32, 4);
            else
                b.chunks.gpu_tile(o, k, oo, ko, oi, ki, 32, 4);
            rgb(b.chunks);
            Var bk = b.before.args()[0], bo = b.before.args()[1], bc = b.before.args()[2];
            b.before.compute_root().reorder(bc, bk, bo).gpu_tile(bo, oo, oi, 64);
            b.before.update().reorder(bc, b.scan.x, bo).gpu_tile(bo, oo, oi, 64);
            rgb(b.before);
            if (channels > 1)
                b.before.update().vectorize(bc);
            b.out.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            rgb(b.out);
        } else if (b.sliding) {
            // One thread per run of pixels: along x, runs of a row; along y, runs of a
            // column (neighbouring threads read neighbouring pixels).
            Var run("run"), i("i"), t("t"), to("to"), ti("ti");
            Var u = b.along_x ? x : y, other = b.along_x ? y : x;
            b.out.compute_root().bound(c, 0, lanes).reorder_storage(c, x, y);
            b.out.split(u, run, i, SLIDE, TailStrategy::GuardWithIf);
            if (b.along_x)
                b.out.reorder(c, i, run, y).gpu_tile(run, y, xo, yo, xi, yi, 16, 16);
            else
                b.out.reorder(c, i, x, run).gpu_tile(x, run, xo, yo, xi, yi, 16, 16);
            if (channels > 1)
                b.out.vectorize(c);
            b.running.compute_at(b.out, xi);
            b.first.compute_at(b.out, xi);
            if (channels > 1) {
                b.running.bound(b.running.args()[3], 0, lanes).vectorize(b.running.args()[3]);
                b.running.update().vectorize(b.running.args()[3]);
            }
        } else if (gpu && channels > 1) {
            // Channels as float4, stored together: one load and one add per tap for all
            // channels (a fourth channel is computed and unused for RGB).
            b.out.compute_root().bound(c, 0, 4).reorder_storage(c, x, y).reorder(c, x, y).vectorize(
                c);
            if (staged) {
                const int tw = b.along_x ? 64 : 8, th = b.along_x ? 4 : 32;
                b.out.gpu_tile(x, y, xo, yo, xi, yi, tw, th);
                b.first.compute_at(b.out, xi).bound(c, 0, 4).vectorize(c);
                b.first.update().vectorize(c);
                // Loaded by the block's own threads (as many as its pixels: a stage with
                // more would make the block too large for the GPU, and it would not run).
                Func load = b.input.in(b.first);
                Var lx = load.args()[0], ly = load.args()[1], lc = load.args()[2];
                Var lxo("lxo"), lyo("lyo"), lxi("lxi"), lyi("lyi");
                load.compute_at(b.out, xo).store_in(MemoryType::GPUShared).bound(lc, 0, 4);
                load.reorder_storage(lc, lx, ly).reorder(lc, lx, ly).vectorize(lc);
                load.tile(lx, ly, lxo, lyo, lxi, lyi, tw, th, TailStrategy::GuardWithIf)
                    .gpu_threads(lxi, lyi);
            } else {
                b.out.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
                b.first.compute_at(b.out, xi).bound(c, 0, 4).vectorize(c);
                b.first.update().vectorize(c);
            }
        } else if (gpu) {
            b.out.compute_root().bound(c, 0, 1).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            b.first.compute_at(b.out, xi);
        } else {
            b.out.bound(c, 0, channels);
            b.out.compute_root().reorder(x, y, c).vectorize(x, 8).parallel(y);
            b.out.specialize(b.radius == 0); // no running sum to compute there
            // The running sum. Along x: one row at a time, inside the parallel rows of
            // the box. Along y: a whole channel, columns in parallel and vectorised.
            if (b.along_x) {
                b.running.compute_at(b.out, y);
            } else {
                b.running.compute_at(b.out, c);
                b.running.update().split(x, xo, xi, 64).parallel(xo).vectorize(xi, 8);
            }
        }
    }
}

} // namespace ms_blur
