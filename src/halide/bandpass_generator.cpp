// MS_BandPass in Halide: the difference of two 3-box blurs of the Source, mixed over it by
// the mask value (ms_mask, mask_generator.cpp), compiled ahead of time for the CPU and for
// the GPU. Built into C functions (see CMakeLists.txt) that effects/ms_bandpass.c calls.
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "blur.h"

using namespace Halide;
using ms_blur::blur3;
using ms_blur::Box;

namespace {

Var x("x"), y("y"), c("c");

} // namespace

class BandPass : public Generator<BandPass> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<Buffer<float, 2>> mask_value{"mask_value"}; // ms_mask; none: 1 (strides 0)
    Input<int> blur1_x{"blur1_x"}, blur1_y{"blur1_y"}; // total radii in pixels (rounded)
    Input<int> blur2_x{"blur2_x"}, blur2_y{"blur2_y"};
    Input<float> brightness{"brightness"}, saturation{"saturation"}, offset_darks{"offset_darks"};
    Output<Buffer<float, 3>> output{"output"};
    // GPU: windows read from on-chip memory (blur.h); the "staged" versions, for box radii
    // up to ms_blur::STAGED_RADIUS_X / _Y (effects/ms_bandpass.c).
    GeneratorParam<bool> staged{"staged", false};
    // GPU: windows as differences of running sums (blur.h): the same cost whatever the blur;
    // the "prefix" versions, for large blurs (effects/ms_bandpass.c).
    GeneratorParam<bool> prefix{"prefix", false};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Expr width = source.width(), height = source.height();

        // Blur 1 and blur 2 of the Source's RGB (alpha is not used).
        Func src("src");
        src(x, y, c) = source(x, y, c);
        std::vector<Box> rgb;
        Func blur1 = blur3(src, blur1_x, blur1_y, width, height, gpu, "blur1_", rgb, false, prefix);
        Func blur2 = blur3(src, blur2_x, blur2_y, width, height, gpu, "blur2_", rgb, false, prefix);

        // The band-pass: difference of the blurs, saturation, brightness, offset darks,
        // mixed over the Source by the mask; alpha unchanged.
        Func d("d");
        d(x, y, c) = blur1(x, y, c) - blur2(x, y, c);
        Func l("l");
        l(x, y) = d(x, y, 0) * 0.3125f + d(x, y, 1) * 0.5f + d(x, y, 2) * 0.1875f;
        Expr rgb_c = min(c, 2); // alpha (c = 3) is the Source's: the blurs have RGB only
        Expr ds = l(x, y) + saturation * (d(x, y, rgb_c) - l(x, y));
        Expr effect = (2.0f * brightness) * ds + offset_darks * (1.0f - 2.0f * ds);
        Expr mk = mask_value(x, y);
        Expr s = source(x, y, c);
        output(x, y, c) = select(c == 3, s, s * (1.0f - mk) + effect * mk);

        // Interleaved RGBA: channels next to each other.
        source.dim(0).set_stride(4);
        source.dim(2).set_stride(1).set_bounds(0, 4);
        mask_value.dim(0).set_stride(Expr()); // without a mask: one value (stride 0)
        output.dim(0).set_stride(4);
        output.dim(2).set_stride(1).set_bounds(0, 4);

        // Schedule: each box computed whole (RGB: 3 channels); d and l per pixel of the
        // output. (On the GPU, sliding windows (blur.h) were slower here than summing each
        // window: 213 vs 147 ms at 4K; staged windows: 82 ms.)
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(rgb, gpu, 3, staged);
        if (gpu) {
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        } else {
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
        }
    }
};

HALIDE_REGISTER_GENERATOR(BandPass, ms_bandpass)
HALIDE_REGISTER_GENERATOR(BandPass, ms_bandpass_gpu) // for the staged and prefix versions
