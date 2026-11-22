// MS_JpegDamage: the image as a JPEG file of a low quality shows it: 8 x 8 blocks, lost
// detail, chroma at half resolution (src/halide/jpeg_damage_generator.cpp: the IJG library's
// way, in integers).
//   - Quality q: the library's quality 100 q, its standard tables scaled by it (5000 / Q under
//     50, else 200 - 2 Q, in percent), steps from 1 to 32767 (not limited to 255)
//   - the Frequency settings scale the dequantised coefficients (the decoder's steps: the
//     step x the gain truncated, kept in 16 bits (modulo 65536, as a 16-bit table); the
//     products in 32-bit integers, unlimited): All
//     every one but the mean; X those across (any horizontal frequency u: by the setting
//     below 1, by 1 + (s - 1) (u - v) / u above), Y the same down; Low, Mid, High by
//     max(u, v): 0-1 low, 2 half low half mid, 3-4 mid, 5 half mid half high, 6-7 high;
//     their product g taken as g^a on the luma (a: Affect Luma) and on the chroma (a: Affect
//     Chroma)
//   - Saturation, Scale Lights and Offset Darks on the result; the Mask mixes it over the
//     Source
//   - Res Factor r: the image compressed at max(floor(r Res Rel X), 1) x floor(r) times less
//     resolution (each cell its bottom-left pixel, no mean), then back cell by cell
//   - the errors (src/halide/jpeg_damage_generator.cpp): MillSaph's own random, new every
//     Jitter Frames frames (0: never)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/mask_input.h"
#include "engine/engine.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
};
enum { SOURCE, MASK };

enum {
    QUALITY,
    RES_FACTOR,
    RES_REL_X,
    ALL_FREQ,
    X_FREQ,
    Y_FREQ,
    LOW_FREQ,
    MID_FREQ,
    HIGH_FREQ,
    AFFECT_LUMA,
    AFFECT_CHROMA,
    ERROR_RATE,
    ERROR_DENSITY,
    ERROR_AMP,
    ERROR_COHERENCE,
    JITTER_FRAMES,
    RAND_SEED,
    SCALE_LIGHTS,
    OFFSET_DARKS,
    SATURATION,
    FLIP_NOISE,
    MASK_USE,
    BLUR_MASK,
    INVERT_MASK
};
#define SCALE(id, n, l, h)                                                                         \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .def = 1.0,                                                                            \
            .min = 0.0,                                                                            \
            .max = MS_UNBOUNDED,                                                                   \
            .display_max = 10.0}
static const MsParam params[] = {
    [QUALITY] = {.type = MS_PARAM_DOUBLE,
                 .name = "quality",
                 .label = "Quality",
                 .hint = "The JPEG quality: low values, larger losses.",
                 .def = 0.1,
                 .min = 0.01,
                 .max = 1.0,
                 .display_min = 0.01,
                 .display_max = 1.0},
    [RES_FACTOR] = {.type = MS_PARAM_DOUBLE,
                    .name = "res_factor",
                    .label = "Res Factor",
                    .hint = "Compresses the image at this many times less resolution.",
                    .def = 1.0,
                    .min = 1.0,
                    .max = MS_UNBOUNDED,
                    .display_min = 1.0,
                    .display_max = 32.0},
    [RES_REL_X] = {.type = MS_PARAM_DOUBLE,
                   .name = "res_rel_x",
                   .label = "Res Rel X",
                   .hint = "Scales the Res Factor horizontally.",
                   .def = 1.0,
                   .min = 0.01,
                   .max = MS_UNBOUNDED,
                   .display_min = 0.01,
                   .display_max = 5.0},
    SCALE(ALL_FREQ, "all_freq_scale", "All Freq Scale", "Scales every frequency of the blocks."),
    SCALE(X_FREQ, "x_freq_scale", "X Freq Scale", "Scales the blocks' horizontal frequencies."),
    SCALE(Y_FREQ, "y_freq_scale", "Y Freq Scale", "Scales the blocks' vertical frequencies."),
    SCALE(LOW_FREQ, "low_freq_scale", "Low Freq Scale", "Scales the blocks' low frequencies."),
    SCALE(MID_FREQ, "mid_freq_scale", "Mid Freq Scale", "Scales the blocks' middle frequencies."),
    SCALE(HIGH_FREQ, "high_freq_scale", "High Freq Scale", "Scales the blocks' high frequencies."),
    [AFFECT_LUMA] = {.type = MS_PARAM_DOUBLE,
                     .name = "affect_luma",
                     .label = "Affect Luma",
                     .hint = "How much the Frequency settings change the luma (0: not at all).",
                     .def = 1.0,
                     .min = 0.0,
                     .max = MS_UNBOUNDED,
                     .display_max = 10.0},
    [AFFECT_CHROMA] = {.type = MS_PARAM_DOUBLE,
                       .name = "affect_chroma",
                       .label = "Affect Chroma",
                       .hint =
                           "How much the Frequency settings change the colours (0: not at all).",
                       .def = 0.5,
                       .min = 0.0,
                       .max = MS_UNBOUNDED,
                       .display_max = 10.0},
    [ERROR_RATE] = {.type = MS_PARAM_DOUBLE,
                    .name = "error_rate",
                    .label = "Error Rate",
                    .hint = "How many errors damage the blocks (0: none).",
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 5.0},
    MS_FRACTION(ERROR_DENSITY, "err_block_density", "Err Block Density",
                "The share of the blocks the errors can reach.", 0.75),
    [ERROR_AMP] = {.type = MS_PARAM_DOUBLE,
                   .name = "error_amp",
                   .label = "Error Amp",
                   .hint = "How strong the errors are.",
                   .def = 1.0,
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 5.0},
    [ERROR_COHERENCE] = {.type = MS_PARAM_DOUBLE,
                         .name = "error_coherence",
                         .label = "Error Coherence",
                         .hint = "How much the errors run on from block to block.",
                         .def = 1.0,
                         .min = 0.0,
                         .max = MS_UNBOUNDED,
                         .display_max = 5.0},
    [JITTER_FRAMES] = {.type = MS_PARAM_DOUBLE,
                       .name = "jitter_frames",
                       .label = "Jitter Frames",
                       .hint = "The errors change every this many frames (0: never).",
                       .def = 1.0,
                       .min = 0.0,
                       .max = MS_UNBOUNDED,
                       .display_max = 30.0},
    [RAND_SEED] = {.type = MS_PARAM_DOUBLE,
                   .name = "rand_seed",
                   .label = "Rand Seed",
                   .hint = "Another pattern of errors for each value.",
                   .def = 0.123,
                   .min = -MS_UNBOUNDED,
                   .max = MS_UNBOUNDED,
                   .display_max = 5.0},
    [SCALE_LIGHTS] = {.type = MS_PARAM_DOUBLE,
                      .name = "scale_lights",
                      .label = "Scale Lights",
                      .hint = "Scales the result's whites.",
                      .def = 1.0,
                      .min = -MS_UNBOUNDED,
                      .max = MS_UNBOUNDED,
                      .display_max = 5.0},
    [OFFSET_DARKS] = {.type = MS_PARAM_DOUBLE,
                      .name = "offset_darks",
                      .label = "Offset Darks",
                      .hint = "Raises (or lowers) the result's blacks.",
                      .min = -MS_UNBOUNDED,
                      .max = MS_UNBOUNDED,
                      .display_min = -5.0,
                      .display_max = 1.0},
    [SATURATION] = {.type = MS_PARAM_DOUBLE,
                    .name = "saturation",
                    .label = "Saturation",
                    .hint = "The result's saturation (0: grey).",
                    .def = 1.0,
                    .min = -MS_UNBOUNDED,
                    .max = MS_UNBOUNDED,
                    .display_max = 2.0},
    [FLIP_NOISE] = {.type = MS_PARAM_BOOL,
                    .name = "flip_noise_vertically",
                    .label = "Flip Noise Vertically",
                    .hint = "Turns the pattern of the errors upside down."},
    [MASK_USE] = MS_PARAM_MASK_USE,
    [BLUR_MASK] = {.type = MS_PARAM_WIDTH,
                   .name = "blur_mask",
                   .label = "Blur Mask",
                   .hint = "Blurs the Mask input by this amount before using.",
                   .def = 0.00625,
                   .min = 0.0,
                   .max = MS_UNBOUNDED,
                   .display_max = 0.2},
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Damages the black areas of the Mask."),
};

// The IJG library's standard tables (luminance, chrominance), rows v.
static const int standard[2][64] = {
    {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
     14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
     18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
     49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99},
    {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
     99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99}};

// One Frequency setting's gain for (u, v) along one axis (the setting's own, a its index,
// b the other's): below 1 every frequency with a > 0; above, as a dominates.
static double axis_gain(double s, int a, int b) {
    if (s <= 1.0)
        return a >= 1 ? s : 1.0;
    return a > b ? 1.0 + (s - 1.0) * (a - b) / (double)a : 1.0;
}

// The quantisation steps and the decoder's, from Quality and the Frequency settings: as the
// kernel's images (v rows of u, the luma's and chroma's next to each other; whole numbers).
static void tables(const double *v, float steps[8][8][2], float decoder[8][8][2]) {
    int q = (int)floor(v[QUALITY] * 100.0 + 0.5);
    q = q < 1 ? 1 : q > 100 ? 100 : q;
    const long scale = q < 50 ? 5000 / q : 200 - 2 * q;
    for (int t = 0; t < 2; ++t)
        for (int vv = 0; vv < 8; ++vv)
            for (int u = 0; u < 8; ++u) {
                long s = (standard[t][vv * 8 + u] * scale + 50) / 100;
                s = s < 1 ? 1 : s > 32767 ? 32767 : s;
                const int r = u > vv ? u : vv;
                const double low = r <= 1 ? 1.0 : r == 2 ? 0.5 : 0.0;
                const double high = r >= 6 ? 1.0 : r == 5 ? 0.5 : 0.0;
                double gain =
                    (low * v[LOW_FREQ] + (1.0 - low - high) * v[MID_FREQ] + high * v[HIGH_FREQ]) *
                    v[ALL_FREQ] * axis_gain(v[X_FREQ], u, vv) * axis_gain(v[Y_FREQ], vv, u);
                // Affect Luma / Affect Chroma a on the table's plane: above 1, 1 + a (g - 1);
                // below, g^a (at gain 0, any a > 0 the same)
                const double a = v[t == 0 ? AFFECT_LUMA : AFFECT_CHROMA];
                gain = gain >= 1.0 ? 1.0 + a * (gain - 1.0) : pow(fmax(gain, 0.0), a);
                if (u == 0 && vv == 0)
                    gain = 1.0;
                steps[vv][u][t] = (float)s;
                decoder[vv][u][t] = (float)fmod(floor(s * gain), 65536.0);
            }
}

// One render: images and settings of the kernels (src/halide/jpeg_damage_generator.cpp,
// mask_generator.cpp).
typedef struct {
    MsImage *source, *steps, *decoder, *output;
    MsMask *mask;
    int fx, fy, seed, epoch, flip;
    float rate, amp, density, coherence;
    float saturation, lights, darks;
} Job;

static int run(MsApi api, void *user) {
    const Job *j = user;
    const int status = ms_mask_run(j->mask, api);
    if (status != 0)
        return status;
    return MS_KERNEL(MS_K_JPEG_DAMAGE, api, MS_IMAGE("source", j->source),
                     MS_IMAGE("mask_value", &j->mask->value), MS_IMAGE("steps", j->steps),
                     MS_IMAGE("decoder", j->decoder), MS_INT("fx", j->fx), MS_INT("fy", j->fy),
                     MS_FLOAT("rate", j->rate), MS_FLOAT("amp", j->amp),
                     MS_FLOAT("density", j->density), MS_FLOAT("coherence", j->coherence),
                     MS_INT("seed", j->seed), MS_INT("epoch", j->epoch), MS_INT("flip", j->flip),
                     MS_FLOAT("saturation", j->saturation), MS_FLOAT("lights", j->lights),
                     MS_FLOAT("darks", j->darks), MS_IMAGE("output", j->output));
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height;
    MsImage source, steps_image, decoder_image, output;
    ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
    MsMask mask;
    if (!ms_mask_init(&mask, r, MASK, SOURCE, &source, v[MASK_USE], v[INVERT_MASK], v[BLUR_MASK]))
        return 0;
    float steps[8][8][2], decoder[8][8][2];
    tables(v, steps, decoder);
    ms_image(&steps_image, &steps[0][0][0], 16, 8, 8, 2);
    ms_image(&decoder_image, &decoder[0][0][0], 16, 8, 8, 2);
    ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
    Job j = {.source = &source,
             .mask = &mask,
             .steps = &steps_image,
             .decoder = &decoder_image,
             .output = &output,
             .saturation = (float)v[SATURATION],
             .lights = (float)v[SCALE_LIGHTS],
             .darks = (float)v[OFFSET_DARKS]};
    const double res = floor(fmin(fmax(v[RES_FACTOR], 1.0), 4096.0));
    j.fy = (int)res;
    j.fx = (int)fmax(floor(fmin(res * v[RES_REL_X], 4096.0)), 1.0);
    j.rate = (float)v[ERROR_RATE];
    j.amp = (float)v[ERROR_AMP];
    // (the threshold of the kernel's noise (two uniforms blended) under which a share
    // density^1.6 of it is, as the reference's damaged share; a fit of its distribution)
    const double share = pow(fmin(fmax(v[ERROR_DENSITY], 0.0), 1.0), 1.6);
    const double threshold =
        ((((3.8314 * share - 9.5912) * share + 9.3131) * share - 4.3698) * share + 1.7861) * share +
        0.015;
    j.density = (float)(share <= 0.0 ? -1.0 : share >= 1.0 ? 2.0 : threshold);
    j.coherence = (float)v[ERROR_COHERENCE];
    // (MillSaph's random; + 5: at the default seed, damage about as wide as the reference's)
    j.seed = (int)floor(fmod(v[RAND_SEED] * 1000.0, 2147483647.0)) + 5;
    j.epoch = v[JITTER_FRAMES] > 0 ? (int)floor(fmod(r->time / v[JITTER_FRAMES], 2147483647.0)) : 0;
    j.flip = (int)v[FLIP_NOISE];
    // Inputs uploaded to the GPU: the Source, the Mask or the one mask value, the tables.
    MsImage *const inputs[] = {&source, mask.input, &steps_image, &decoder_image};
    MsImage *between[] = {&mask.value}; // made on the GPU from the Mask
    const int ok =
        ms_run(run, &j, inputs, 4, between, mask.has_mask, &output, r->info, r->err, r->err_size);
    ms_mask_free(&mask);
    return ok;
}

const MsEffect ms_jpegdamage_effect = {
    MS_ID("JpegDamage"),
    "MS_JpegDamage",
    "Damages the image as a low-quality JPEG file does: blocks, lost detail, blurred colours.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
    NULL,
    1, // the errors change over time (Jitter Frames)
    NULL,
    NULL,
    0,
    "jpegdamage", // presets (common/preset.h)
    NULL,         // not varying with its settings
};
