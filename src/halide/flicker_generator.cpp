// MS_Flicker in Halide, called by effects/ms_flicker.c: the Source's colour times a gain per
// channel that changes from frame to frame, the same over the whole image:
//   gain_c = Brightness + Amplitude x amp_c x (Rand Luma Amp x N_luma + Rand Color Amp x N_c
//            + Wave Amp x cos(2 pi (wave_t - phase_c)))
// N: MillSaph's own noise over time (one for the luma, one per channel, by the Seed), the sum
// of two value noises (uniform -1..1 on a lattice, smoothstep between) scaled to sd 0.315,
// read at the lattice cell and the fraction ms_flicker.c works out from the time (the
// reference's noise: band-limited, sd 0.315, correlation 0.88 / 0.6 / 0.3 / 0 at 0.21 / 0.42 /
// 0.62 / 1 of its time unit). Alpha unchanged.
//
// Images are packed RGBA floats (x, y, c), interleaved, bottom row first.

#include "Halide.h"
#include "noise.h"
#include "shared.h"

using namespace Halide;

namespace {

Var x("x"), y("y"), c("c");

using ms_noise::u32;

// -1..1 from a 32-bit hash.
Expr symmetric(Expr h) { return cast<float>(h >> u32(8)) * (2.0f / 16777216.0f) - 1.0f; }

// A value noise between lattice points `cell` and `cell + 1`, `u` (0..1) past the first.
Expr value_noise(Expr cell, Expr u, Expr salt, Expr seed) {
    Expr a = symmetric(ms_noise::hash3(cell, salt, seed));
    Expr b = symmetric(ms_noise::hash3(cell + 1, salt, seed));
    return a + (b - a) * (u * u * (3.0f - 2.0f * u));
}

// The two value noises' sum (sd 0.497 x sqrt 2 each pair) to sd 0.315.
const float NOISE_SCALE = 0.448f;

} // namespace

class Flicker : public Generator<Flicker> {
  public:
    Input<Buffer<float, 3>> source{"source"};
    Input<int> cell{"cell"};  // the noise's lattice cell at this frame
    Input<float> u{"u"};      // and the fraction past it
    Input<int> seed{"seed"};
    Input<float> amplitude{"amplitude"}, luma_amp{"luma_amp"}, color_amp{"color_amp"};
    Input<float> wave_amp{"wave_amp"};
    Input<float> wave_t{"wave_t"}; // the wave's cycles at this frame (its fraction)
    Input<float> phase_r{"phase_r"}, phase_g{"phase_g"}, phase_b{"phase_b"};
    Input<float> amp_r{"amp_r"}, amp_g{"amp_g"}, amp_b{"amp_b"};
    Input<float> brightness{"brightness"};
    Output<Buffer<float, 3>> output{"output"};

    void generate() {
        const bool gpu = get_target().has_gpu_feature();

        // Noise k: 0 the luma, 1 to 3 the channels.
        auto noise = [&](Expr k) {
            return NOISE_SCALE * (value_noise(cell, u, 2 * k, seed) +
                                  value_noise(cell, u, 2 * k + 1, seed));
        };
        Expr phase = select(c == 0, phase_r, c == 1, phase_g, phase_b);
        Expr amp = select(c == 0, amp_r, c == 1, amp_g, amp_b);
        Expr wave = cos(6.28318530718f * (wave_t - phase));
        Expr g = brightness + amplitude * amp *
                                  (luma_amp * noise(0) + color_amp * noise(c + 1) + wave_amp * wave);
        Func gain("gain");
        gain(c) = select(c < 3, g, 1.0f);

        output(x, y, c) = source(x, y, c) * gain(c);

        ms::interleaved(source);
        ms::interleaved(output);

        Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
        gain.bound(c, 0, 4).compute_root();
        if (gpu) {
            gain.gpu_single_thread();
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        } else {
            output.bound(c, 0, 4).reorder(c, x, y).unroll(c).vectorize(x, 8).parallel(y);
        }
    }
};

HALIDE_REGISTER_GENERATOR(Flicker, ms_flicker)
