// The Mask input of the effects, in Halide. Images are packed RGBA floats, interleaved.

#include "Halide.h"
#include "blur.h"
#include "mask.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

} // namespace

// The mask value of the effects: luma or alpha of the Mask input, maybe inverted, blurred by
// Blur Mask (mask.h). Run only when a Mask is connected.
class Mask : public Generator<Mask> {
  public:
    Input<Buffer<float, 3>> mask{"mask"};
    Input<int> mask_alpha{"mask_alpha"}, invert_mask{"invert_mask"};
    Input<int> blur_mask{"blur_mask"}; // total radius in pixels
    Output<Buffer<float, 2>> mask_value{"mask_value"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();
        std::vector<ms_blur::Box> boxes;
        Func m("m");
        m(x, y, c) = ms_mask::value(mask, x, y, mask_alpha, invert_mask);
        Func mb = ms_blur::blur3(m, blur_mask, blur_mask, mask.width(), mask.height(), gpu, "maskb_",
                                 boxes, false, true);
        mask_value(x, y) = mb(x, y, 0);

        mask.dim(0).set_stride(4);
        mask.dim(2).set_stride(1).set_bounds(0, 4);
        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        ms_blur::schedule(boxes, gpu, 1);
        if (gpu)
            mask_value.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        else
            mask_value.vectorize(x, 8).parallel(y);
    }
};

HALIDE_REGISTER_GENERATOR(Mask, ms_mask)
