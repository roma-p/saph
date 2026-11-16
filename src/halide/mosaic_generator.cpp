// MS_Mosaic in Halide: every pixel takes the colour of its cell, a bilinear sample of the
// (smoothed) Source at the cell's centre, mixed over the Source by the mask value
// (mask_generator.cpp), sampled at the same place. Called by effects/ms_mosaic.c.
//
// The grid (all in pixels, x right and y up, pixel i covering [i, i + 1)):
//   - cells of cell_w x cell_h from the grid's origin (the image's centre, shifted); a pixel
//     whose centre is on a cell edge goes to the cell further from the origin
//   - the sample point is the cell's centre, in index coordinates (pixel i at i), reflected
//     into the kept part of the image [lo, hi] (the image less its crop) back and forth
//   - bilinear weights in 1/256 steps: the fractions kx, ky rounded to 1/256, the weight of
//     pixel (x0, y0 + 1) ((256 - kx) ky + 127) >> 8, the other three what makes each row and
//     column of the 2 x 2 weights sum to theirs; the pixels read clamped into the kept part
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

// The sample point of each pixel's cell along one axis: the pixel to its left (or below),
// and the fraction past it in 1/256 (0 to 256). `offset`: the cell's offset from the origin,
// computed apart (scheduled below) so that no backend fuses it with the addition that
// follows (a fused multiply-add rounds once, not twice: another 1/256 step at times).
struct Axis {
    Func offset, first, fraction;
};

Axis axis(const std::string &name, Var u, Expr size, Expr origin, Expr kept_min, Expr kept_max) {
    Axis a{Func(name + "_offset"), Func(name + "_first"), Func(name + "_fraction")};
    Expr d = cast<float>(u) + 0.5f - origin;
    Expr k = select(d >= 0.0f, floor(d / size), -floor(-d / size) - 1.0f);
    a.offset(u) = (k + 0.5f) * size;
    Expr centre = a.offset(u) + origin - 0.5f;
    // Reflected into [lo, hi] back and forth (period 2 (hi - lo); whole numbers times whole
    // numbers there, exact).
    Expr lo = cast<float>(kept_min), span = cast<float>(kept_max + 1 - kept_min);
    Expr t = centre - lo;
    t = t - 2.0f * span * floor(t / (2.0f * span));
    t = select(t > span, 2.0f * span - t, t);
    Expr p = lo + t;
    a.first(u) = cast<int>(floor(p));
    a.fraction(u) = cast<int>(round((p - floor(p)) * 256.0f));
    return a;
}

} // namespace

class Mosaic : public Generator<Mosaic> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 3>> sampled{"sampled"};       // the Source, or it smoothed
    Input<Buffer<float, 2>> mask_value{"mask_value"}; // ms_mask; none: 1 (strides 0)
    Input<float> cell_w{"cell_w"}, cell_h{"cell_h"};  // cell size in pixels
    Input<float> origin_x{"origin_x"}, origin_y{"origin_y"};
    Input<int> kept_x0{"kept_x0"}, kept_x1{"kept_x1"}; // pixels kept by the crop, included
    Input<int> kept_y0{"kept_y0"}, kept_y1{"kept_y1"};
    Input<float> mix{"mix"}; // Mix With Source
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Axis ax = axis("ax", x, cell_w, origin_x, kept_x0, kept_x1);
        Axis ay = axis("ay", y, cell_h, origin_y, kept_y0, kept_y1);

        // The four weights, in 1/256.
        Expr kx = ax.fraction(x), ky = ay.fraction(y);
        Expr w01 = ((256 - kx) * ky + 127) >> 8;
        Expr w00 = 256 - kx - w01;
        Expr w11 = ky - w01;
        Expr w10 = 256 - w00 - w01 - w11;
        Func weights("weights");
        weights(x, y) = Tuple(cast<float>(w00) * (1.0f / 256), cast<float>(w10) * (1.0f / 256),
                              cast<float>(w01) * (1.0f / 256), cast<float>(w11) * (1.0f / 256));

        Expr x0 = clamp(ax.first(x), kept_x0, kept_x1);
        Expr x1 = clamp(ax.first(x) + 1, kept_x0, kept_x1);
        Expr y0 = clamp(ay.first(y), kept_y0, kept_y1);
        Expr y1 = clamp(ay.first(y) + 1, kept_y0, kept_y1);
        auto sample = [&](auto at) {
            return at(x0, y0) * weights(x, y)[0] + at(x1, y0) * weights(x, y)[1] +
                   at(x0, y1) * weights(x, y)[2] + at(x1, y1) * weights(x, y)[3];
        };
        Expr cell = sample([&](Expr u, Expr v) { return sampled(u, v, c); });
        // The mask goes through the mosaic too.
        Expr mk = sample([&](Expr u, Expr v) { return mask_value(u, v); });
        Expr s = source(x, y, c);
        output(x, y, c) = s + (cell - s) * (mk * (1.0f - mix));

        ms::interleaved(source);
        ms::interleaved(sampled);
        mask_value.dim(0).set_stride(Expr()); // without a mask: one value (stride 0)
        ms::interleaved(output);

        // Schedule: the sample points per column and per row first; the weights per pixel
        // (inlined).
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi"), uo("uo"), ui("ui");
        for (Axis *a : {&ax, &ay}) {
            Var u = a->first.args()[0];
            a->offset.compute_root();
            a->first.compute_root();
            a->fraction.compute_root();
            if (gpu) {
                a->offset.gpu_tile(u, uo, ui, 64);
                a->first.gpu_tile(u, uo, ui, 64);
                a->fraction.gpu_tile(u, uo, ui, 64);
            } else {
                a->offset.vectorize(u, 8);
                a->first.vectorize(u, 8);
                a->fraction.vectorize(u, 8);
            }
        }
        ms::schedule_rgba(output, gpu);
    }
};

HALIDE_REGISTER_GENERATOR(Mosaic, ms_mosaic)
