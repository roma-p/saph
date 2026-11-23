// The 3-box blur of the effects: 3 boxes along x, then 3 along y, borders mirrored (pixel -1
// = pixel 0, repeated for a box wider than the image). The total radius (rounded by the
// caller) is split into 3 near-equal box radii.

#pragma once

#include "Halide.h"
#include "ms_limits.h"
#include "shared.h"

namespace ms_blur {

using namespace Halide;

// How the GPU boxes sum their windows (the CPU's: a running sum in double):
//   windows  every pixel sums its window
//   staged   the same, the block's pixels and windows loaded first into on-chip memory
//   sliding  each thread slides the window along a run of pixels (one channel)
//   prefix   differences of running sums: the same cost whatever the radius
enum class Boxes { windows, staged, sliding, prefix };

// One box: the mean over [i - r, i + r] along x or y.
struct Box {
    Func out;
    Func running; // CPU: running sum along the line; GPU sliding: along a run of pixels
    Func first;   // GPU sliding: the window of the first pixel of each run; GPU windows: the sum
    bool along_x;
    Boxes kind; // on the GPU
    Expr radius;
    Func input; // GPU windows: the mirrored input
    // GPU prefix: the running sums of the lines.
    std::shared_ptr<ms::LineSums> sums;
};

// A box beyond the plain one, for the prefix boxes (GPU) and the CPU's only (all fields
// optional):
//   r_max  the radius r may differ per channel (an Expr of the channel): its largest value,
//          without the channel
//   frac   between two radii: the mean over [i - r, i + r] and the one over
//          [i - r - 1, i + r + 1] mixed by frac (0: the first)
//   zero   != 0: the pixels outside the image are 0 instead of mirrored
struct Shape {
    Expr r_max, frac, zero;
};

// GPU sliding boxes: each thread slides the window along a run of SLIDE pixels (adds the
// pixel entering it, subtracts the one leaving it): about 2 + (2r + 1) / SLIDE reads per
// pixel instead of 2r + 1, for large radii.
constexpr int SLIDE = 32;

// GPU prefix boxes: each window is the difference of two running sums of the line
// (ms::LineSums, chunks of CHUNK pixels), from pixel -r - 1 on: the same cost whatever the
// radius. In float (Metal has no double): the line's first pixel is taken off every pixel
// first, to keep the sums small.
constexpr int CHUNK = 16;

inline Box box(Func in, Expr r, bool along_x, Expr width, Expr height, bool gpu, Boxes kind,
               const std::string &name, const Shape &shape = {}) {
    Var x = in.args()[0], y = in.args()[1], c = in.args()[2];
    const bool mixed = shape.frac.defined();
    Func mirrored = BoundaryConditions::mirror_image(in, {{0, width}, {0, height}});
    Func edge = mirrored;
    if (shape.zero.defined()) {
        Func zeros = BoundaryConditions::constant_exterior(in, 0.0f, {{0, width}, {0, height}});
        edge = Func(name + "_edge");
        edge(x, y, c) = select(shape.zero != 0, zeros(x, y, c), mirrored(x, y, c));
    }
    Expr r_max = shape.r_max.defined() ? shape.r_max : r;
    Expr reach = mixed ? r_max + 1 : r_max; // the widest window's radius
    Expr frac = mixed ? shape.frac : Expr(0.0f);
    Box b{Func(name), Func(name + "_sum"), Func(name + "_first"), along_x, kind, r_max};
    auto at = [&](Expr along, Expr other) {
        return along_x ? edge(along, other, c) : edge(other, along, c);
    };
    Expr along = along_x ? Expr(x) : Expr(y), other = along_x ? Expr(y) : Expr(x);
    Expr length = along_x ? width : height;
    // The mean of the box from the window sums w0 (radius r) and w1 (r + 1).
    auto mean = [&](std::function<Expr(Expr)> window) {
        Expr w0 = window(r);
        if (!mixed)
            return w0 / cast<float>(2 * r + 1);
        Expr w1 = window(r + 1);
        return (1.0f - frac) * (w0 / cast<float>(2 * r + 1)) + frac * (w1 / cast<float>(2 * r + 3));
    };
    if (gpu && kind == Boxes::prefix) {
        // The lines from pixel -reach - 1 (along: u from there; across: o), the line's first
        // pixel taken off.
        Var u("u"), o("o");
        Expr start = -reach - 1;
        Func line(name + "_line");
        line(u, o, c) = at(start + u, o) - at(0, o);
        // The sums inside chunks kept when each pixel reads two windows (between two radii),
        // else summed by each pixel.
        b.sums = std::make_shared<ms::LineSums>(name, line, length + 2 * reach + 1, !along_x, mixed,
                                                CHUNK);
        auto sum_to = [&](Expr p) { return b.sums->at(p - start, other, c); };
        Expr m = mean(
            [&](Expr rr) { return ms::difference(sum_to(along + rr), sum_to(along - rr - 1)); });
        b.out(x, y, c) = select(r == 0 && frac == 0.0f, edge(x, y, c), m + at(0, other));
    } else if (gpu && kind == Boxes::sliding) {
        // run(i, run index, other coordinate, c): window sum of pixel run x SLIDE + i.
        Var i("i"), run("run"), o("o");
        RDom k(-r, 2 * r + 1, name + "_k");
        RDom j(1, SLIDE - 1, name + "_j");
        b.first(run, o, c) = sum(at(run * SLIDE + k, o));
        b.running(i, run, o, c) = b.first(run, o, c);
        b.running(j, run, o, c) = b.running(j - 1, run, o, c) + at(run * SLIDE + j + r, o) -
                                  at(run * SLIDE + j - 1 - r, o);
        // Radius 0: the input itself (sliding adds and subtracts pixels: in float, not
        // exactly the pixel, and the Distort shift magnifies any difference of its lens).
        // Read mirrored: Halide's bounds for these boxes can reach past the image.
        b.out(x, y, c) =
            select(r == 0, mirrored(x, y, c),
                   b.running(along % SLIDE, along / SLIDE, other, c) / cast<float>(2 * r + 1));
    } else if (gpu) {
        // GPU: every pixel sums its own window: fully parallel. (A running sum per piece
        // of line was tried: barely faster here, less precise, as Metal has no double.)
        RDom k(0, 2 * r + 1, name + "_k");
        Expr xs = along_x ? x + k - r : Expr(x);
        Expr ys = along_x ? Expr(y) : y + k - r;
        b.first(x, y, c) = 0.0f; // the window sum
        b.first(x, y, c) += mirrored(xs, ys, c);
        b.out(x, y, c) = b.first(x, y, c) / cast<float>(2 * r + 1);
        b.input = mirrored;
    } else {
        // CPU: a running sum along the line (in double), then each window is the difference
        // of two sums: the same cost whatever the radius.
        RDom k(-reach - 1, length + 2 * reach + 1, name + "_k");
        Func &s = b.running;
        s(x, y, c) = cast<double>(0);
        if (along_x)
            s(k, y, c) = s(k - 1, y, c) + cast<double>(edge(k, y, c));
        else
            s(x, k, c) = s(x, k - 1, c) + cast<double>(edge(x, k, c));
        auto sum_to = [&](Expr u) { return along_x ? s(u, y, c) : s(x, u, c); };
        Expr m =
            mean([&](Expr rr) { return cast<float>(sum_to(along + rr) - sum_to(along - rr - 1)); });
        // Radius 0 (e.g. no blur at all): the input itself, without the running sum (see
        // schedule()).
        b.out(x, y, c) = select(r == 0 && frac == 0.0f, edge(x, y, c), m);
    }
    return b;
}

// The 3 box radii of a blur of total radius `total`.
inline Expr box_radius(Expr total, int i) { return total / 3 + select(i >= 3 - total % 3, 1, 0); }

// The 3-box blur of `in` (x, y, c): 3 horizontal boxes, then 3 vertical ones, of `kind` on
// the GPU. Appends its 6 boxes to `boxes` (for schedule()).
inline Func blur3(Func in, Expr total_x, Expr total_y, Expr width, Expr height, bool gpu,
                  const std::string &name, std::vector<Box> &boxes, Boxes kind = Boxes::windows) {
    Func f = in;
    for (int i = 0; i < 6; ++i) {
        const bool along_x = i < 3;
        Expr r = box_radius(along_x ? total_x : total_y, i % 3);
        boxes.push_back(box(f, r, along_x, width, height, gpu, kind, name + std::to_string(i)));
        f = boxes.back().out;
    }
    return f;
}

// The 3-box blur of `in` (x, y, c) along x then y, total radius n + f per axis: f between the
// blurs of n and n + 1, which differ in one box only (radius n / 3 against one more). n and
// f may differ per channel (Exprs of c; n_max: n's largest value, without c). Each box at
// most as wide as the image (radius (length - 1) / 2). Prefix boxes on the GPU; `zero` != 0:
// 0 outside the image, else mirrored. Appends its 6 boxes to `boxes` (for schedule()).
inline Func blur3_subpixel(Func in, Expr nx, Expr fx, Expr nx_max, Expr ny, Expr fy, Expr ny_max,
                           Expr zero, Expr width, Expr height, bool gpu, const std::string &name,
                           std::vector<Box> &boxes) {
    Func f = in;
    for (int axis = 0; axis < 2; ++axis) {
        const bool along_x = axis == 0;
        Expr n = along_x ? nx : ny, frac = along_x ? fx : fy;
        Expr limit = ((along_x ? width : height) - 1) / 2;
        Expr r_max = min((along_x ? nx_max : ny_max) / 3 + 1, limit);
        Expr mixed_box = 2 - n % 3;
        for (int i = 0; i < 3; ++i) {
            Expr r = box_radius(n, i);
            Expr fr = select(i == mixed_box && r + 1 <= limit, frac, 0.0f);
            boxes.push_back(box(f, min(r, limit), along_x, width, height, gpu, Boxes::prefix,
                                name + std::to_string(axis * 3 + i), {r_max, fr, zero}));
            f = boxes.back().out;
        }
    }
    return f;
}

// The same radius (and fraction) on both axes.
inline Func blur3_subpixel(Func in, Expr n, Expr frac, Expr zero, Expr width, Expr height, bool gpu,
                           const std::string &name, std::vector<Box> &boxes) {
    return blur3_subpixel(in, n, frac, n, n, frac, n, zero, width, height, gpu, name, boxes);
}

// GPU RGB staged boxes: each block first loads its pixels and their windows into on-chip
// memory, then sums the windows there. Blocks of 64 x 4 pixels along x, 8 x 32
// along y: on-chip memory (64 + 2r) x 4 or 8 x (32 + 2r) float4, within 32 KB (Metal) for
// box radii up to STAGED_RADIUS_X / _Y (the caller checks).
constexpr int STAGED_RADIUS_X = MS_STAGED_RADIUS_X, STAGED_RADIUS_Y = MS_STAGED_RADIUS_Y;

// Each box computed whole, over `channels` channels (1, or 3 / 4 for RGB(A)).
inline void schedule(std::vector<Box> &boxes, bool gpu, int channels) {
    for (Box &b : boxes) {
        Var x = b.out.args()[0], y = b.out.args()[1], c = b.out.args()[2];
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        const int lanes = channels > 1 ? 4 : 1; // RGB as float4 (a fourth channel unused)
        if (gpu && b.kind == Boxes::prefix) {
            // The running sums (shared.h: along y, neighbouring threads of the windows are on
            // neighbouring lines); the windows: as the other GPU boxes, RGB as float4.
            b.sums->schedule(lanes);
            b.out.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            b.out.bound(c, 0, lanes).reorder_storage(c, x, y);
            if (channels > 1)
                b.out.vectorize(c);
        } else if (gpu && b.kind == Boxes::sliding) {
            // One thread per run of pixels: along x, runs of a row; along y, runs of a
            // column (neighbouring threads read neighbouring pixels). One channel.
            Var run("run"), i("i");
            Var u = b.along_x ? x : y, other = b.along_x ? y : x;
            b.out.compute_root().bound(c, 0, 1).reorder_storage(c, x, y);
            b.out.split(u, run, i, SLIDE, TailStrategy::GuardWithIf);
            if (b.along_x)
                b.out.reorder(c, i, run, y).gpu_tile(run, y, xo, yo, xi, yi, 16, 16);
            else
                b.out.reorder(c, i, x, run).gpu_tile(x, run, xo, yo, xi, yi, 16, 16);
            b.running.compute_at(b.out, xi);
            b.first.compute_at(b.out, xi);
        } else if (gpu && channels > 1) {
            // Channels as float4, stored together: one load and one add per tap for all
            // channels (a fourth channel is computed and unused for RGB).
            b.out.compute_root().bound(c, 0, 4).reorder_storage(c, x, y).reorder(c, x, y).vectorize(
                c);
            if (b.kind == Boxes::staged) {
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
