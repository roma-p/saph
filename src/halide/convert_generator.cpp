// Images of 16-bit floats ("half", as hosts store them) to float and back, for the engine
// (engine/run.c): the host's images go to the GPU in half, are converted there, and the
// result back. The same conversions as src/common/half.c, in 32-bit integer operations every
// API has (Vulkan and Direct3D 12 may have no 16-bit types): a pixel's four halves are read
// as two 32-bit integers, R and B in their low 16 bits (little-endian).

#include "Halide.h"

using namespace Halide;

namespace {

// The layout of the images: `values` per pixel next to each other (4 floats, or 2 integers of
// two halves); the schedule: one pixel per thread on the GPU.
void layout(OutputImageParam image, int values) {
    image.dim(0).set_stride(values);
    image.dim(2).set_stride(1).set_bounds(0, values);
}

Expr u32(uint32_t value) { return Internal::make_const(UInt(32), (uint64_t)value); }

void schedule(Func output, int values, const Target &target) {
    Var x = output.args()[0], y = output.args()[1], c = output.args()[2];
    Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
    output.bound(c, 0, values).reorder(c, x, y).unroll(c);
    if (target.has_gpu_feature())
        output.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
    else
        output.vectorize(x, 8).parallel(y);
}

} // namespace

class ToFloat : public Generator<ToFloat> {
  public:
    Input<Buffer<uint32_t, 3>> input{"input"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        Var x("x"), y("y"), c("c");
        Expr h = (input(x, y, c / 2) >> (16 * (c % 2))) & 0xffff;
        Expr sign = (h & 0x8000) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
        // Zero and denormals: mantissa x 2^-24; infinities and NaN; the others.
        Expr small = cast<float>(mantissa) * 5.9604644775390625e-8f;
        Expr bits = select(exponent == 31, sign | 0x7f800000 | (mantissa << 13),
                           sign | ((exponent + 112) << 23) | (mantissa << 13));
        output(x, y, c) =
            select(exponent == 0, select(sign != 0, -small, small), reinterpret<float>(bits));
        layout(input, 2);
        layout(output, 4);
        schedule(output, 4, get_target());
    }
};

class ToHalf : public Generator<ToHalf> {
  public:
    Input<Buffer<float, 3>> input{"input"};
    Output<Buffer<uint32_t, 3>> output{"output"};

    // The bits of the half nearest `f`, in the low 16 bits.
    static Expr half_of(Expr f) {
        Expr all = reinterpret<uint32_t>(f);
        Expr sign = (all >> 16) & 0x8000, u = all & 0x7fffffff;
        // 65536 and above, infinities, NaN; below the smallest normal half (adding 0.5
        // puts the denormal half's bits at the bottom of the float, rounded); the others,
        // rounded to the nearest even.
        Expr large = select(u > 0x7f800000, u32(0x7e00), u32(0x7c00));
        Expr denormal =
            reinterpret<uint32_t>(strict_float(reinterpret<float>(u) + 0.5f)) - u32(0x3f000000);
        Expr normal = (u + u32(0xc8000fff) + ((u >> 13) & 1)) >> 13;
        return sign | select(u >= 0x47800000, large, u < 0x38800000, denormal, normal);
    }

    void generate() {
        Var x("x"), y("y"), c("c");
        output(x, y, c) = half_of(input(x, y, 2 * c)) | (half_of(input(x, y, 2 * c + 1)) << 16);
        layout(input, 4);
        layout(output, 2);
        schedule(output, 2, get_target());
    }
};

HALIDE_REGISTER_GENERATOR(ToFloat, ms_to_float)
HALIDE_REGISTER_GENERATOR(ToHalf, ms_to_half)
