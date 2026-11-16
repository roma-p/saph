// What the generators share: the layout of the images, and the compensated running sums of
// the GPU versions that sum whole lines (LineSums: blur.h's prefix boxes, dt_generator.cpp's
// RowSums).

#pragma once

#include "Halide.h"

namespace ms {

using namespace Halide;

// Interleaved RGBA: channels next to each other.
template <typename B> void interleaved(B &&b) {
    b.dim(0).set_stride(4);
    b.dim(2).set_stride(1).set_bounds(0, 4);
}

// An RGBA output (x, y, c) computed per pixel, its 4 channels unrolled: 16 x 16 GPU tiles, or
// 8-wide vectors and parallel rows on the CPU.
template <typename F> void schedule_rgba(F &&f, bool gpu) {
    Var x = f.args()[0], y = f.args()[1], c = f.args()[2];
    Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
    f.bound(c, 0, 4).reorder(c, x, y).unroll(c);
    if (gpu)
        f.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
    else
        f.vectorize(x, 8).parallel(y);
}

// Running sums over a line in float (Metal has no double), chunk by chunk: the sum of the
// chunks before each chunk is compensated (Kahan: the sum, and what its rounding left out),
// strict (kept as is). The next (sum, left out) from the previous ones and the next chunk.
inline Tuple kahan_add(Expr sum, Expr left_out, Expr chunk) {
    Expr add = chunk - left_out;
    Expr total = strict_float(sum + add);
    return Tuple(total, strict_float((total - sum) - add));
}

// A running sum at a sample: the sum of the chunks before its chunk, what its rounding left
// out, and its chunk up to it.
struct Compensated {
    Expr chunks, left_out, inside;
};

// hi - lo: the large sums of chunks first, so that the rounding of their difference is about
// the difference's size, not theirs.
inline Expr difference(const Compensated &hi, const Compensated &lo) {
    return strict_float(((hi.chunks - lo.chunks) - (hi.left_out - lo.left_out)) +
                        (hi.inside - lo.inside));
}

// Running sums along lines in GPU memory, for differences of sums at any distance. In chunks
// of `chunk` samples: per line, the sum of the chunks before each chunk, compensated (one
// thread per line), and per sample the sum inside its chunk up to it, re-reading its chunk.
// `kept`: those sums inside chunks computed once, one thread per sample (for readers of many
// sums per pixel); else each reader sums its chunks itself, and only the chunks' sums are
// kept (one thread per chunk). `lines_inner`: stored with neighbouring lines next to each
// other, for readers whose neighbouring threads are on neighbouring lines (else neighbouring
// samples).
struct LineSums {
    Func value, inside, chunks, before;
    RDom scan; // the chunks, in before's update
    int chunk;
    bool channels, lines_inner, kept;

    // inside's or before's coordinates: (j, k, line[, c]) or (k, line[, c]).
    static std::vector<Expr> args(std::vector<Expr> a, Expr c) {
        if (c.defined())
            a.push_back(c);
        return a;
    }

    // value(i, line[, c]): sample i of a line, for i in [0, length).
    LineSums(const std::string &name, Func value, Expr length, bool lines_inner, bool kept,
             int chunk = 16)
        : value(value), inside(name + "_inside"), chunks(name + "_chunks"),
          before(name + "_before"), scan(1, (length + chunk - 1) / chunk, name + "_scan"),
          chunk(chunk), channels(value.dimensions() == 3), lines_inner(lines_inner), kept(kept) {
        Var j("j"), k("k"), o("o"), c("c");
        Expr ch = channels ? Expr(c) : Expr();
        if (kept) {
            inside(args({j, k, o}, ch)) = summed(j, k, o, ch);
            chunks(args({k, o}, ch)) = inside(args({chunk - 1, k, o}, ch));
        } else {
            chunks(args({k, o}, ch)) = summed(chunk - 1, k, o, ch);
        }
        before(args({k, o}, ch)) = Tuple(0.0f, 0.0f);
        FuncRef last = before(args({scan - 1, o}, ch));
        before(args({scan, o}, ch)) = kahan_add(last[0], last[1], chunks(args({scan - 1, o}, ch)));
    }

    // The sum of samples 0 to j of chunk k of `line`, in turn. (An Expr of the reader's
    // coordinates only: through a Func, Halide would compute it over the whole range of
    // chunks the reader's windows span.)
    Expr summed(Expr j, Expr k, Expr line, Expr c) const {
        RDom i(0, chunk);
        return sum(select(i <= j, value(args({k * chunk + i, line}, c)), 0.0f));
    }

    // The running sum up to sample i of `line`, included.
    Compensated at(Expr i, Expr line, Expr c = Expr()) const {
        Expr k = i / chunk, j = i % chunk;
        return {before(args({k, line}, c))[0], before(args({k, line}, c))[1],
                kept ? Expr(inside(args({j, k, line}, c))) : summed(j, k, line, c)};
    }

    // Each stage whole, on the GPU; `lanes` channels (4: as float4).
    void schedule(int lanes = 4) {
        Var ko("ko"), oo("oo"), ki("ki"), oi("oi"), jk("jk");
        // f's loops and storage, from the innermost: the channels, then the samples
        // (`along`, f's first coordinates) and the line, or the line first (lines_inner).
        auto place = [&](Func f, std::vector<Var> along) {
            std::vector<Var> a = f.args(), order = along;
            Var line = a[along.size()];
            order.insert(lines_inner ? order.begin() : order.end(), line);
            if (channels) {
                f.bound(a.back(), 0, lanes);
                order.insert(order.begin(), a.back());
            }
            f.compute_root().reorder_storage(order).reorder(
                std::vector<VarOrRVar>(order.begin(), order.end()));
            if (channels && lanes > 1)
                f.vectorize(a.back());
            return line;
        };
        if (kept) {
            Var j = inside.args()[0], k = inside.args()[1];
            inside.bound(j, 0, chunk);
            Var o = place(inside, {j, k});
            inside.fuse(j, k, jk);
            if (lines_inner)
                inside.gpu_tile(o, jk, oo, ko, oi, ki, 64, 4);
            else
                inside.gpu_tile(jk, o, ko, oo, ki, oi, 64, 4);
        } else {
            Var k = chunks.args()[0];
            Var o = place(chunks, {k});
            if (lines_inner)
                chunks.gpu_tile(o, k, oo, ko, oi, ki, 32, 4);
            else
                chunks.gpu_tile(k, o, ko, oo, ki, oi, 32, 4);
        }
        Var bo = place(before, {before.args()[0]});
        before.gpu_tile(bo, oo, oi, 64);
        Stage sum = before.update();
        if (channels) {
            Var bc = before.args().back();
            sum.reorder(bc, scan.x, bo);
            if (lanes > 1)
                sum.vectorize(bc);
        } else {
            sum.reorder(scan.x, bo);
        }
        sum.gpu_tile(bo, oo, oi, 64);
    }
};

} // namespace ms
