// MS_Flicker: the image's brightness and colour flickering over time, the same gain per
// channel over the whole frame (src/halide/flicker_generator.cpp):
//   gain_c = Brightness + Amplitude x amp_c x (Rand Luma Amp x N_luma + Rand Color Amp x N_c
//            + Wave Amp x cos(2 pi (Wave Freq x t / 24 - phase_c)))
//   - t: the frame; the times are in 24ths of a second whatever the project's rate (as the
//     reference's)
//   - N: random over time, at Rand Freq x t / 24 (Rand Freq 0: one value), one noise for the
//     luma and one per channel, by the Seed
//   - alpha unchanged

#include <math.h>

#include "common/defines.h"
#include "common/effect.h"
#include "effects/input.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
};
enum { SOURCE };

enum {
    AMPLITUDE,
    LUMA_AMP,
    COLOR_AMP,
    RAND_FREQ,
    WAVE_AMP,
    WAVE_FREQ,
    PHASE_R,
    PHASE_G,
    PHASE_B,
    AMP_R,
    AMP_G,
    AMP_B,
    BRIGHTNESS,
    SEED
};

// A scale from 0, slider to 2.
#define AMP(id, n, l, h, d)                                                                        \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .def = (d),                                                                            \
            .min = 0.0,                                                                            \
            .max = MS_UNBOUNDED,                                                                   \
            .display_max = 2.0}
// A frequency (per second), slider to 60.
#define FREQ(id, n, l, h, d)                                                                       \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .def = (d),                                                                            \
            .min = 0.0,                                                                            \
            .max = MS_UNBOUNDED,                                                                   \
            .display_max = 60.0}
// A phase (in turns), -1 to 1.
#define PHASE(id, n, l, h)                                                                         \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .min = -MS_UNBOUNDED,                                                                  \
            .max = MS_UNBOUNDED,                                                                   \
            .display_min = -1.0,                                                                   \
            .display_max = 1.0}

static const MsParam params[] = {
    MS_FRACTION(AMPLITUDE, "amplitude", "Amplitude", "How much the image flickers.", 0.2),
    AMP(LUMA_AMP, "rand_luma_amp", "Rand Luma Amp",
        "The random flicker of the brightness (the three channels together).", 1.0),
    AMP(COLOR_AMP, "rand_color_amp", "Rand Color Amp",
        "The random flicker of each colour channel apart.", 0.0),
    FREQ(RAND_FREQ, "rand_freq", "Rand Freq",
         "How fast the random flicker changes (about the most changes per second).", 30.0),
    AMP(WAVE_AMP, "wave_amp", "Wave Amp", "A regular flicker, a cosine wave over time.", 0.0),
    FREQ(WAVE_FREQ, "wave_freq", "Wave Freq", "The wave's cycles per second.", 5.0),
    PHASE(PHASE_R, "wave_r_phase", "Wave R Phase", "Delays the wave in red (in cycles)."),
    PHASE(PHASE_G, "wave_g_phase", "Wave G Phase", "Delays the wave in green (in cycles)."),
    PHASE(PHASE_B, "wave_b_phase", "Wave B Phase", "Delays the wave in blue (in cycles)."),
    AMP(AMP_R, "red_amp", "Red Amp", "Scales the flicker in red.", 1.0),
    AMP(AMP_G, "green_amp", "Green Amp", "Scales the flicker in green.", 1.0),
    AMP(AMP_B, "blue_amp", "Blue Amp", "Scales the flicker in blue.", 1.0),
    [BRIGHTNESS] = {.type = MS_PARAM_DOUBLE,
                    .name = "brightness",
                    .label = "Brightness",
                    .hint = "Scales the image, flicker added.",
                    .def = 1.0,
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 5.0},
    MS_RELATIVE(SEED, "seed", "Seed", "Another random flicker.", 0.123),
};

// The noise's lattice points per unit of Rand Freq x t / 24 (sets how fast it changes, as
// the reference's).
#define NOISE_LATTICE (4.0 / 3.0)

// One render: images and settings of the kernel.
typedef struct {
    MsImage *source, *output;
    int cell, seed;
    float u, amplitude, luma_amp, color_amp, wave_amp, wave_t, phase[3], amp[3], brightness;
} Job;

static int run(MsApi api, void *user) {
    const Job *j = user;
    return MS_KERNEL(MS_K_FLICKER, api, MS_IMAGE("source", j->source), MS_INT("cell", j->cell),
                     MS_FLOAT("u", j->u), MS_INT("seed", j->seed),
                     MS_FLOAT("amplitude", j->amplitude), MS_FLOAT("luma_amp", j->luma_amp),
                     MS_FLOAT("color_amp", j->color_amp), MS_FLOAT("wave_amp", j->wave_amp),
                     MS_FLOAT("wave_t", j->wave_t), MS_FLOAT("phase_r", j->phase[0]),
                     MS_FLOAT("phase_g", j->phase[1]), MS_FLOAT("phase_b", j->phase[2]),
                     MS_FLOAT("amp_r", j->amp[0]), MS_FLOAT("amp_g", j->amp[1]),
                     MS_FLOAT("amp_b", j->amp[2]), MS_FLOAT("brightness", j->brightness),
                     MS_IMAGE("output", j->output));
}

static int render(const MsRender *r) {
    const double *v = r->values;
    MsImage source, output;
    ms_input_image(&source, r, SOURCE);
    ms_output_image(&output, r);
    // Where the noise and the wave are at this frame, in double (long sequences); the cell
    // kept in the hash's range.
    const double at = fmax(v[RAND_FREQ], 0.0) * r->time / 24.0 * NOISE_LATTICE;
    const double cell = floor(at), cycles = v[WAVE_FREQ] * r->time / 24.0;
    Job j = {.source = &source,
             .output = &output,
             .cell = (int)fmod(cell, 1073741824.0),
             .u = (float)(at - cell),
             .seed = (int)floor(v[SEED] * 1000.0 + 0.5),
             .amplitude = (float)v[AMPLITUDE],
             .luma_amp = (float)v[LUMA_AMP],
             .color_amp = (float)v[COLOR_AMP],
             .wave_amp = (float)v[WAVE_AMP],
             .wave_t = (float)(cycles - floor(cycles)),
             .phase = {(float)v[PHASE_R], (float)v[PHASE_G], (float)v[PHASE_B]},
             .amp = {(float)v[AMP_R], (float)v[AMP_G], (float)v[AMP_B]},
             .brightness = (float)v[BRIGHTNESS]};
    MsImage *const inputs[] = {&source};
    return ms_run(run, &j, inputs, 1, NULL, 0, &output, r->info, r->err, r->err_size);
}

// A gain of 1 in every channel: the Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    const int still = v[AMPLITUDE] == 0.0 ||
                      (v[LUMA_AMP] == 0.0 && v[COLOR_AMP] == 0.0 && v[WAVE_AMP] == 0.0) ||
                      (v[AMP_R] == 0.0 && v[AMP_G] == 0.0 && v[AMP_B] == 0.0);
    return still && v[BRIGHTNESS] == 1.0;
}

const MsEffect ms_flicker_effect = {
    .id = MS_ID("Flicker"),
    .label = "MS_Flicker",
    .description =
        "Makes the image flicker over time: its brightness and colours change randomly, or as a "
        "wave, from frame to frame.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    .identity = identity,
    .frame_varying = 1,
    .presets = "flicker",
};
