// The Atmosphere (MS_LensFlare, MS_Glow): 1 + amp x MillSaph's noise over the frame, octaves
// of gradient noise from about 4 cycles across the frame (x Atmosphere Freq) down to 2
// pixels, each Atmosphere Detail x the one before; amp 1: a standard deviation of about 0.2 at
// the default detail (as the reference's), changing slowly over time (x Atmosphere Speed);
// 0.5575 = 0.157 / 0.2816, the first octave's sd. The kernels take these settings
// (ms_noise::fbm at (x + 0.5, y + 0.5) x freq, time z).

#pragma once

#include <math.h>

#include "common/effect.h"

// Its five parameters, entries `first` to `first` + 4: Atmosphere Amp (its hint `amp_hint`),
// Freq, Detail, Seed, Speed (MsParam initializers).
#define MS_ATMOSPHERE_PARAMS(first, amp_hint)                                                      \
    MS_RELATIVE(first, "atmosphere_amp", "Atmosphere Amp", amp_hint, 0.0),                         \
        [(first) + 1] = {.type = MS_PARAM_DOUBLE,                                                  \
                         .name = "atmosphere_freq",                                                \
                         .label = "Atmosphere Freq",                                               \
                         .hint = "The size of the Atmosphere's clouds (higher: smaller).",         \
                         .def = 1.0,                                                               \
                         .min = 0.01,                                                              \
                         .max = MS_UNBOUNDED,                                                      \
                         .display_min = 0.1,                                                       \
                         .display_max = 20.0},                                                     \
                   MS_FRACTION((first) + 2, "atmosphere_detail", "Atmosphere Detail",              \
                               "How much fine detail the Atmosphere has.", 0.6),                   \
                   [(first) + 3] = {.type = MS_PARAM_DOUBLE,                                       \
                                    .name = "atmosphere_seed",                                     \
                                    .label = "Atmosphere Seed",                                    \
                                    .hint = "Another Atmosphere for each value.",                  \
                                    .def = 0.123,                                                  \
                                    .display_max = 5.0},                                           \
                   [(first) + 4] = {.type = MS_PARAM_DOUBLE,                                       \
                                    .name = "atmosphere_speed",                                    \
                                    .label = "Atmosphere Speed",                                   \
                                    .hint = "How fast the Atmosphere changes over time.",          \
                                    .def = 1.0,                                                    \
                                    .display_min = -5.0,                                           \
                                    .display_max = 5.0}

typedef struct {
    float amp, freq, persistence, z;
    int octaves; // 0 without Atmosphere
} MsAtmosphere;

// From Atmosphere Amp, Freq, Detail, Seed, Speed, on a frame `frame_width` pixels wide at
// frame `time`.
static inline MsAtmosphere ms_atmosphere(double amp, double freq, double detail, double seed,
                                         double speed, double frame_width, double time) {
    MsAtmosphere a = {0};
    const double cycles = 4.0 * fmax(freq, 1e-3);
    if (amp > 0) {
        const int octaves = (int)floor(log2(fmax(frame_width / (2.0 * cycles), 1.0))) + 1;
        a.octaves = octaves < 12 ? octaves : 12;
    }
    a.amp = (float)(0.5575 * amp);
    a.freq = (float)(cycles / fmax(frame_width, 1.0));
    a.persistence = (float)detail;
    a.z = (float)(fmod(0.032 * speed * time + 37.1 * seed, 4096.0));
    return a;
}
