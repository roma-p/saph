// MS_FlickerMatch and MS_FlickerMatchColor in Halide, called by effects/ms_flickermatch.c:
// the Source scaled by the Match input's level in a rectangle over the target level, so that
// the Source takes the Match input's flicker:
//   - the mean of each channel of the Match input over the rectangle's pixels (sums per
//     column, then over the columns: compensated running sums in float)
//   - FlickerMatch: one gain for the three channels, the mean's luma (5 R + 8 G + 3 B) / 16
//     over Match Level; FlickerMatchColor: a gain per channel, its mean over Match Color's
//   - colour channels times their gain (a target of 0 or less: unchanged); alpha unchanged
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), c("c");

} // namespace

class FlickerMatch : public Generator<FlickerMatch> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 3>> match{"match"};
    // The rectangle: its first pixel and its size, inside the Match input.
    Input<int> x0{"x0"}, y0{"y0"}, columns{"columns"}, rows{"rows"};
    Input<int> luma{"luma"}; // 1: one gain from the luma (FlickerMatch)
    Input<float> target_r{"target_r"}, target_g{"target_g"}, target_b{"target_b"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Var y("y");

        // The sum of each column of the rectangle (and what its rounding left out).
        RDom ry(0, rows, "ry");
        Func column("column");
        column(x, c) = Tuple(0.0f, 0.0f);
        column(x, c) = ms::kahan_add(column(x, c)[0], column(x, c)[1], match(x0 + x, y0 + ry, c));

        // Their sum.
        RDom rx(0, columns, "rx");
        Func total("total");
        total(c) = Tuple(0.0f, 0.0f);
        total(c) = ms::kahan_add(total(c)[0], total(c)[1],
                                 strict_float(column(rx, c)[0] - column(rx, c)[1]));

        Func mean("mean");
        mean(c) = strict_float(total(c)[0] - total(c)[1]) /
                  (cast<float>(columns) * cast<float>(rows));

        Func gain("gain");
        Expr level = (5.0f * mean(0) + 8.0f * mean(1) + 3.0f * mean(2)) / 16.0f;
        Expr target = select(c == 0, target_r, c == 1, target_g, target_b);
        Expr g = select(luma != 0, level, mean(c)) / target;
        gain(c) = select(c < 3 && target > 0.0f, g, 1.0f);

        output(x, y, c) = source(x, y, c) * gain(c);

        ms::interleaved(source);
        ms::interleaved(match);
        ms::interleaved(output);

        // Schedule: the columns one thread each (rows in turn), the total and the gains in
        // one thread, the pixels.
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        column.bound(c, 0, 4).compute_root();
        total.bound(c, 0, 4).compute_root();
        total.update().reorder(c, rx).unroll(c);
        gain.bound(c, 0, 4).compute_root();
        if (gpu) {
            column.reorder_storage(c, x).reorder(c, x).gpu_tile(x, xo, xi, 64);
            column.update().reorder(c, ry, x).unroll(c).gpu_tile(x, xo, xi, 64);
            total.gpu_single_thread();
            total.update().gpu_single_thread();
            gain.gpu_single_thread();
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        } else {
            column.update()
                .split(x, xo, xi, 8, TailStrategy::GuardWithIf)
                .reorder(xi, c, ry, xo)
                .vectorize(xi)
                .unroll(c)
                .parallel(xo);
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
        }
    }
};

HALIDE_REGISTER_GENERATOR(FlickerMatch, ms_flicker_match)
