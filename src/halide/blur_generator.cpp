// The 3-box blur (blur.h) of an RGBA image, all four channels: for effects that blur their
// Source before using it (MS_Mosaic's Smooth Colors).
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "blur.h"
#include "shared.h"

using namespace Halide;
using ms_blur::Boxes;

namespace {

Var x("x"), y("y"), c("c");

} // namespace

class Blur : public Generator<Blur> {
  public:
    Input<Buffer<float, 3>> input{"input"};
    Input<int> radius_x{"radius_x"}, radius_y{"radius_y"}; // total radii in pixels
    Output<Buffer<float, 3>> output{"output"};
    // GPU boxes (blur.h): every pixel sums its window, or differences of running sums (the
    // same cost whatever the radius, for large blurs); the effect chooses.
    GeneratorParam<Boxes> boxes{
        "boxes", Boxes::windows, {{"windows", Boxes::windows}, {"prefix", Boxes::prefix}}};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        Func in("in");
        in(x, y, c) = input(x, y, c);
        std::vector<ms_blur::Box> rgba;
        Func b = ms_blur::blur3(in, radius_x, radius_y, input.width(), input.height(), gpu, "blur_",
                                rgba, boxes);
        output(x, y, c) = b(x, y, c);

        ms::interleaved(input);
        ms::interleaved(output);
        ms_blur::schedule(rgba, gpu, 4);
        ms::schedule_float4(output, gpu);
    }
};

HALIDE_REGISTER_GENERATOR(Blur, ms_blur)
