// MS_Test in Halide: output = Source x Gain, alpha unchanged. Checks that the kernels run
// (effects/ms_test.c).

#include "Halide.h"
#include "shared.h"

using namespace Halide;

class Gain : public Generator<Gain> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<float> gain{"gain"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        Var x("x"), y("y"), c("c");
        output(x, y, c) = select(c == 3, source(x, y, c), source(x, y, c) * gain);
        ms::interleaved(source);
        ms::interleaved(output);
        ms::schedule_rgba(output, get_target().has_gpu_feature());
    }
};

HALIDE_REGISTER_GENERATOR(Gain, ms_gain)
