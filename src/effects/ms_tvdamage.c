// MS_TVDamage: a damaged TV picture (src/halide/tv_damage_generator.cpp, in order):
//   - reception: ghosts, vertical hold (a roll with a black bar), horizontal hold (rows
//     moved by a fractal noise), fast forward (torn bands)
//   - the orthicon's dark halo around the bright parts; static and interference dots, added;
//     colour stripes, multiplied; the colour correction; bars and tape dropouts, added;
//     scanlines and the vignette, multiplied
//   - turn off, fish eye, TV pixels, then mixed with the Source by the Mask
// Reception Master scales static, interference, ghosts, the holds (from a threshold), bars and
// stripes. Scanlines, vignette, stripes, fish eye, TV pixels and the colour correction are the
// reference's formulas; the random parts (MillSaph's own noise) and the turn off are fitted to
// the reference's scales, sizes and speeds. Sizes scale with the image's width (measured at 320
// pixels), times are in frames (24 per second).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mask.h"
#include "effects/input.h"
#include "effects/mask_input.h"
#include "effects/profile.h"
#include "engine/engine.h"
#include "halide/tv_damage_layout.h"

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Mask", "Mask", 0.0f, 1, 1},
};
enum { SOURCE, MASK };

enum {
    MODE,
    MASTER,
    INTERFERENCE,
    GHOST,
    HHOLD,
    VHOLD,
    BARS,
    STRIPES,
    FAST_FORWARD,
    DROPOUTS,
    VIGNETTE,
    STATIC_AMP,
    STATIC_DENSITY,
    DOTS_FREQ,
    DOTS_SPEED_X,
    DOTS_SPEED_Y,
    JITTER,
    GHOSTS,
    NEGATIVE,
    SPACING,
    VARY,
    SHIFT,
    GHOST_BLUR,
    H_FREQ,
    H_TIME,
    H_OCTAVES,
    H_BORDER,
    V_FREQ,
    V_SPEED,
    V_RANDOM,
    V_BORDER,
    V_DATA,
    BAR_ROLL,
    BAR_SHARP,
    BAR_FREQ,
    BAR1_WIDTH,
    BAR2_REL,
    BAR2_SHARP,
    COLOR_FREQ,
    COLOR_ANGLE,
    ROLL_SPEED,
    FF_FREQ,
    FF_SHIFT,
    FF_HEIGHT,
    DROP_LEN,
    DROP_GAP,
    DROP_FREQ,
    DROP_THRESHOLD,
    DROP_ALWAYS,
    VIG_RADIUS,
    VIG_SOFT,
    VIG_REL_H,
    SCANLINES,
    SCAN_FREQ,
    ORTHICON,
    ORTH_THRESHOLD,
    DARKS_WIDTH,
    HUE,
    SATURATION,
    SCALE_LIGHTS,
    OFFSET_DARKS,
    TINT_LIGHTS,
    TINT_LIGHTS_G,
    TINT_LIGHTS_B,
    TINT_DARKS,
    TINT_DARKS_G,
    TINT_DARKS_B,
    TURN_OFF,
    FLARE_WIDTH,
    FLARE_BRIGHT,
    FADE_TIME,
    FISH_EYE,
    TV_PIXELS,
    DOWNSAMPLE,
    SEED,
    MASK_USE,
    BLUR_MASK,
    INVERT_MASK
};

static const char *const modes[] = {"TVDamage Color", "TVDamage Mono"};

// A slider from lo to hi, its limits min and max.
#define SLIDER(id, n, l, h, d, mn, mx, lo, hi)                                                     \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .def = (d),                                                                            \
            .min = (mn),                                                                           \
            .max = (mx),                                                                           \
            .display_min = (lo),                                                                   \
            .display_max = (hi)}
// An amount from 0, slider to hi.
#define AMOUNT(id, n, l, h, d, hi) SLIDER(id, n, l, h, d, 0.0, MS_UNBOUNDED, 0.0, hi)
#define ANY (-MS_UNBOUNDED)

static const MsParam params[] = {
    [MODE] = {.type = MS_PARAM_CHOICE,
              .name = "Mode",
              .label = "Mode",
              .hint = "A colour or a black and white TV.",
              .entries = modes,
              .entry_count = ARRAY_SIZE(modes)},
    AMOUNT(MASTER, "reception_master", "Reception Master",
           "Scales the reception's damage: static, interference, ghosts, holds, bars, stripes.",
           0.4, 3.0),
    AMOUNT(INTERFERENCE, "interference_amp", "Interference Amp", "Coloured dots of interference.",
           0.6, 5.0),
    AMOUNT(GHOST, "ghost_amp", "Ghost Amp", "Shifted copies of the picture.", 0.6, 5.0),
    SLIDER(HHOLD, "horizontal_hold", "Horizontal Hold",
           "Rows sliding sideways (from a threshold, with Reception Master).", 0.5, 0.0, 1.0, 0.0,
           1.0),
    SLIDER(VHOLD, "vertical_hold", "Vertical Hold",
           "The picture rolling vertically (from a threshold, with Reception Master).", 0.8, 0.0,
           1.0, 0.0, 1.0),
    AMOUNT(BARS, "bars_brightness", "Bars Brightness", "Rolling horizontal bars.", 0.3, 5.0),
    AMOUNT(STRIPES, "color_stripes_amplitude", "Color Stripes Amplitude",
           "Diagonal rainbow stripes.", 0.2, 5.0),
    AMOUNT(FAST_FORWARD, "fast_forward_amount", "Fast Forward Amount",
           "Torn bands, as a tape fast forwarded.", 0.0, 5.0),
    AMOUNT(DROPOUTS, "tape_dropout_brightness", "Tape Dropout Brightness",
           "White streaks on some frames, as a worn tape.", 0.0, 5.0),
    SLIDER(VIGNETTE, "vignette_darkness", "Vignette Darkness", "Darkens the picture's corners.",
           0.0, 0.0, 1.0, 0.0, 1.0),
    AMOUNT(STATIC_AMP, "static_amplitude", "Static Amplitude", "Snow over the picture.", 0.8, 5.0),
    SLIDER(STATIC_DENSITY, "static_density", "Static Density",
           "The share of pixels the static touches (fewer, stronger).", 0.7, 0.01, 1.0, 0.01, 1.0),
    AMOUNT(DOTS_FREQ, "interference_frequency", "Interference Frequency",
           "How many interference dots.", 1.275, 500.0),
    SLIDER(DOTS_SPEED_X, "dots_speed_x", "Dots Speed X", "The dots' move sideways.", 100.0, ANY,
           MS_UNBOUNDED, -200.0, 200.0),
    SLIDER(DOTS_SPEED_Y, "dots_speed_y", "Dots Speed Y", "The dots' move up or down.", -10.0, ANY,
           MS_UNBOUNDED, -200.0, 200.0),
    AMOUNT(JITTER, "jitter_amount", "Jitter Amount", "Moves each row of dots at random.", 10.0,
           1000.0),
    AMOUNT(GHOSTS, "num_ghosts", "Num Ghosts", "How many ghosts.", 5.0, 30.0),
    SLIDER(NEGATIVE, "negative_ghosts", "Negative Ghosts", "The share of ghosts subtracted.", 0.5,
           0.0, 1.0, 0.0, 1.0),
    AMOUNT(SPACING, "spacing", "Spacing", "The distance between ghosts.", 0.2, 5.0),
    SLIDER(VARY, "vary_position", "Vary Position", "Moves each ghost at random.", 0.3, 0.0, 1.0,
           0.0, 1.0),
    SLIDER(SHIFT, "shift", "Shift", "The ghosts' side and distance (0: none).", 0.5, ANY,
           MS_UNBOUNDED, -1.0, 1.0),
    MS_WIDTH(GHOST_BLUR, "blur_", "Blur", "Softens the ghosts across (wide: fainter ghosts).", 0.0,
             0.25),
    AMOUNT(H_FREQ, "h_frequency", "H Frequency", "How many waves down the rows.", 1.25, 5.0),
    AMOUNT(H_TIME, "h_time_vary", "H Time Vary", "How fast the rows' moves change.", 0.5, 5.0),
    SLIDER(H_OCTAVES, "h_octaves", "H Octaves", "Detail of the rows' moves.", 3.0, 1.0, 10.0, 1.0,
           10.0),
    AMOUNT(H_BORDER, "border_width", "Border Width", "The black border where the rows wrap.", 0.05,
           5.0),
    AMOUNT(V_FREQ, "v_frequency", "V Frequency", "How fast the roll's jitter changes.", 2.0, 5.0),
    AMOUNT(V_SPEED, "v_speed", "V Speed", "How fast the picture rolls.", 2.0, 5.0),
    AMOUNT(V_RANDOM, "v_random", "V Random", "The roll's jitter.", 0.1, 5.0),
    SLIDER(V_BORDER, "border_height", "Border Height", "The black bar between rolled pictures.",
           0.1, 0.0, 1.0, 0.0, 1.0),
    AMOUNT(V_DATA, "border_data", "Border Data", "Sync dashes in the bar.", 1.0, 10.0),
    SLIDER(BAR_ROLL, "bar_roll_speed", "Bar Roll Speed", "How fast the bars roll.", 0.5, ANY,
           MS_UNBOUNDED, -10.0, 10.0),
    SLIDER(BAR_SHARP, "bar_sharpness", "Bar Sharpness", "The wide bar's edges.", 0.5, 0.1,
           MS_UNBOUNDED, 0.1, 10.0),
    SLIDER(BAR_FREQ, "bar_frequency", "Bar Frequency", "How many wide bars.", 1.0, 0.1,
           MS_UNBOUNDED, 0.1, 100.0),
    SLIDER(BAR1_WIDTH, "bar1_width", "Bar1 Width", "The wide bar's width.", 0.35, 0.0, 1.0, 0.0,
           1.0),
    SLIDER(BAR2_REL, "bar2_rel_frequency", "Bar2 Rel Frequency",
           "The narrow bars' frequency, relative.", 6.0, 1.0, MS_UNBOUNDED, 1.0, 100.0),
    SLIDER(BAR2_SHARP, "bar2_sharpness", "Bar2 Sharpness", "The narrow bars' edges.", 0.5, 0.01,
           MS_UNBOUNDED, 0.01, 10.0),
    SLIDER(COLOR_FREQ, "color_frequency", "Color Frequency", "Stripes across half the width.", 10.0,
           1.0, MS_UNBOUNDED, 1.0, 40.0),
    SLIDER(COLOR_ANGLE, "color_angle", "Color Angle", "The stripes' angle.", 160.0, ANY,
           MS_UNBOUNDED, -360.0, 360.0),
    AMOUNT(ROLL_SPEED, "roll_speed", "Roll Speed", "How fast the stripes move.", 3.0, 5.0),
    AMOUNT(FF_FREQ, "FF_band_frequency", "FF Band Frequency", "Torn bands down the picture.", 4.0,
           25.0),
    AMOUNT(FF_SHIFT, "FF_band_shift", "FF Band Shift", "How far the bands tear.", 0.1, 5.0),
    SLIDER(FF_HEIGHT, "FF_band_height", "FF Band Height", "The bands' height.", 0.16, 0.0, 1.0, 0.0,
           1.0),
    SLIDER(DROP_LEN, "dropout_length", "Dropout Length", "A streak's length.", 0.25, 0.0, 1.0, 0.0,
           1.0),
    AMOUNT(DROP_GAP, "dropout_gap_length", "Dropout Gap Length", "The gaps between streaks.", 0.2,
           2.0),
    AMOUNT(DROP_FREQ, "dropout_y_freq", "Dropout Y Freq", "How the streaks change down the rows.",
           5.0, 50.0),
    SLIDER(DROP_THRESHOLD, "dropout_y_threshold", "Dropout Y Threshold",
           "Fewer streaky rows when higher.", 0.75, 0.0, 1.0, 0.0, 1.0),
    SLIDER(DROP_ALWAYS, "dropouts_always", "Dropouts Always",
           "How often frames have streaks (0: never).", 1.0, 0.0, 1.0, 0.0, 1.0),
    AMOUNT(VIG_RADIUS, "vignette_radius", "Vignette Radius",
           "Where the vignette darkens, in half widths.", 1.0, 5.0),
    AMOUNT(VIG_SOFT, "vignette_edge_softness", "Vignette Edge Softness", "The vignette's edge.",
           0.5, 5.0),
    SLIDER(VIG_REL_H, "vignette_rel_height", "Vignette Rel Height",
           "The vignette's height, relative to its width.", 0.75, 0.1, MS_UNBOUNDED, 0.1, 5.0),
    SLIDER(SCANLINES, "scanlines", "Scanlines", "Dark lines across.", 0.1, 0.0, 1.0, 0.0, 1.0),
    AMOUNT(SCAN_FREQ, "scanlines_rel_freq", "Scanlines Rel Freq", "How many scanlines.", 1.0, 5.0),
    AMOUNT(ORTHICON, "orthicon", "Orthicon", "Dark halos around the bright parts.", 0.0, 5.0),
    SLIDER(ORTH_THRESHOLD, "threshold", "Threshold", "Where the bright parts start.", 0.7, 0.0, 1.0,
           0.0, 1.0),
    AMOUNT(DARKS_WIDTH, "darks_width", "Darks Width", "The halos' width.", 0.2, 5.0),
    SLIDER(HUE, "hue_shift", "Hue Shift", "Turns the colours (in turns).", 0.0, ANY, MS_UNBOUNDED,
           -1.0, 1.0),
    AMOUNT(SATURATION, "saturation", "Saturation", "The colours' strength.", 1.0, 2.0),
    AMOUNT(SCALE_LIGHTS, "scale_lights", "Scale Lights", "Scales the picture.", 1.0, 5.0),
    SLIDER(OFFSET_DARKS, "offset_darks", "Offset Darks", "Raises (or lowers) the blacks.", 0.0, ANY,
           1.0, -5.0, 1.0),
    MS_COLOUR(TINT_LIGHTS, "tint_lights", "Tint Lights", "Tints the whites."),
    MS_COLOUR_OF(TINT_DARKS, "tint_darks", "Tint Darks", "Tints the blacks.", 0.0),
    SLIDER(TURN_OFF, "turn_off", "Turn Off",
           "Switches the TV off: the picture shrinks to a line, then a fading spot.", 0.0, 0.0, 1.0,
           0.0, 1.0),
    MS_WIDTH(FLARE_WIDTH, "flare_width", "Flare Width", "The turned off spot's width.", 0.19394,
             1.0),
    AMOUNT(FLARE_BRIGHT, "flare_brightness", "Flare Brightness",
           "The turned off spot's brightness.", 2.0, 5.0),
    SLIDER(FADE_TIME, "fade_out_time", "Fade Out Time",
           "The part of Turn Off where the line and spot fade.", 0.7, 0.0, 1.0, 0.0, 1.0),
    SLIDER(FISH_EYE, "fish_eye", "Fish Eye", "Bulges (or pinches) the picture.", 0.0, ANY,
           MS_UNBOUNDED, -5.0, 5.0),
    SLIDER(TV_PIXELS, "tv_pixels", "Tv Pixels", "The TV's pixels across (Downsample).", 720.0, 1.0,
           MS_UNBOUNDED, 1.0, 1440.0),
    [DOWNSAMPLE] = {.type = MS_PARAM_BOOL,
                    .name = "downsample",
                    .label = "Downsample",
                    .hint = "Shows the picture at the TV's pixels.",
                    .def = 1.0},
    MS_RELATIVE(SEED, "seed", "Seed", "Other random damage.", 0.123),
    [MASK_USE] = MS_PARAM_MASK_USE,
    [BLUR_MASK] = MS_PARAM_BLUR_MASK(0.00667, 0.2),
    [INVERT_MASK] = MS_PARAM_INVERT_MASK("Damages the black areas of the Mask."),
};

// Static Density: the share of pixels touched and their gain (the reference's, measured;
// linear between).
static void static_share(double density, double *share, double *boost) {
    static const double at[][3] = {{0.0, 0.0, 6.0},    {0.05, 0.001, 5.8}, {0.1, 0.009, 5.4},
                                   {0.2, 0.052, 2.7},  {0.3, 0.129, 1.22}, {0.5, 0.351, 1.1},
                                   {0.7, 0.609, 1.05}, {0.9, 0.873, 1.01}, {1.0, 1.0, 1.0}};
    const int n = ARRAY_SIZE(at);
    density = fmin(fmax(density, 0.0), 1.0);
    for (int i = 1; i < n; ++i)
        if (density <= at[i][0]) {
            const double t = (density - at[i - 1][0]) / (at[i][0] - at[i - 1][0]);
            *share = at[i - 1][1] + t * (at[i][1] - at[i - 1][1]);
            *boost = at[i - 1][2] + t * (at[i][2] - at[i - 1][2]);
            return;
        }
    *share = 1.0;
    *boost = 1.0;
}

// Ghost Blur b (pixels; k: the image's width / 320): each ghost spread across by a gaussian of
// sd b / 24, cut at 1 pixel (2 from sd 1.2), and fainter from b = 20 k (none from 120 k), as
// the reference's (the ghost of a line at 320 wide: sides 0.13, 0.21, 0.25 at b 12, 16, 20;
// then 0.87, 0.75, 0.55, 0.44, 0.14 of it at 24, 32, 48, 64, 80).
static void ghost_blur(double b, double k, float *s) {
    static const double at[] = {20, 24, 32, 48, 64, 80, 120},
                        kept[] = {1, 0.874, 0.746, 0.553, 0.436, 0.135, 0};
    const double sd = fmax(b, 0.0) / 24.0, ref = fmax(b, 0.0) / fmax(k, 1e-6);
    double w1 = 0.0, w2 = 0.0;
    if (sd > 0.05) {
        w1 = exp(-0.5 / (sd * sd));
        w2 = sd >= 1.2 ? exp(-2.0 / (sd * sd)) : 0.0;
    }
    const double total = 1.0 + 2.0 * w1 + 2.0 * w2;
    double fade = ref <= at[0] ? 1.0 : 0.0;
    for (int i = 0; i + 1 < ARRAY_SIZE(at); ++i)
        if (ref > at[i] && ref <= at[i + 1])
            fade = kept[i] + (kept[i + 1] - kept[i]) * (ref - at[i]) / (at[i + 1] - at[i]);
    s[TV_GHOST_WEIGHT] *= (float)fade;
    s[TV_GHOST_BLUR1] = (float)(w1 / total);
    s[TV_GHOST_BLUR2] = (float)(w2 / total);
}

// The settings row (tv_damage_layout.h) from the parameters, for an image w x h at `time`.
static void settings(const double *v, int w, int h, double time, float *s) {
    const double k = w / 320.0, master = fmax(v[MASTER], 0.0);
    s[TV_TIME] = (float)time;
    s[TV_SEED] = (float)floor(v[SEED] * 1000.0 + 0.5);
    s[TV_MONO] = (float)(v[MODE] == 1.0);
    s[TV_WIDTH] = (float)w;
    double share, boost;
    static_share(v[STATIC_DENSITY], &share, &boost);
    s[TV_STATIC_AMP] = (float)(master * v[STATIC_AMP]);
    s[TV_STATIC_SHARE] = (float)share;
    s[TV_STATIC_BOOST] = (float)boost;
    s[TV_DOTS_AMP] = (float)(master * v[INTERFERENCE]);
    s[TV_DOTS_SHARE] = (float)fmin(0.0048 * fmax(v[DOTS_FREQ], 0.0), 1.0);
    s[TV_DOTS_W] = s[TV_DOTS_H] = (float)fmax(1.5 * k, 1.0);
    s[TV_DOTS_X] = (float)fmod(time * (7.0 + 0.065 * v[DOTS_SPEED_X]) * k, 65536.0);
    s[TV_DOTS_Y] = (float)fmod(time * 0.06 * v[DOTS_SPEED_Y] * k, 65536.0);
    s[TV_DOTS_JITTER] = (float)(0.02 * v[JITTER] * k);
    // Ghosts (the reference's): spread evenly over Spacing x the width, from -(1 - Shift) / 2
    // of it to (1 + Shift) / 2 (Shift within -1..1; one ghost: in the middle); raw weights
    // 0.97 x Ghost Amp x master x a random number in 0..1, each moved by Vary Position x
    // (a random number - 0.5) x the spread.
    const double ghost = master * fmax(v[GHOST], 0.0);
    const double span = fmax(v[SPACING], 0.0) * w, shift = fmin(fmax(v[SHIFT], -1.0), 1.0);
    s[TV_GHOSTS] = ghost > 0.0 ? (float)fmin(floor(v[GHOSTS] + 0.5), 30.0) : 0.0f;
    s[TV_GHOST_WEIGHT] = (float)(0.97 * ghost);
    s[TV_GHOST_START] = (float)(-(1.0 - shift) * span / 2.0);
    s[TV_GHOST_SPAN] = (float)span;
    s[TV_GHOST_VARY] = (float)(fmax(v[VARY], 0.0) * span);
    s[TV_GHOST_NEGATIVE] = (float)fmin(fmax(v[NEGATIVE], 0.0), 1.0);
    ghost_blur(v[GHOST_BLUR], k, s);
    // Horizontal hold: from master x hold 0.1, 28 pixels per unit at 320.
    const double hold = fmax(master * v[HHOLD] - 0.1, 0.0) * 28.0 * k;
    s[TV_HHOLD_AMP] = (float)hold;
    s[TV_HHOLD_FREQ] = (float)(2.0 * v[H_FREQ]);
    s[TV_HHOLD_Z] = (float)fmod(time / 24.0 * 2.0 * v[H_TIME] + 17.1 * s[TV_SEED], 4096.0);
    s[TV_HHOLD_OCTAVES] = (float)fmin(fmax(floor(v[H_OCTAVES] + 0.5), 1.0), 10.0);
    s[TV_HHOLD_BORDER] = hold > 0.0 ? (float)(0.25 * v[H_BORDER] * w) : 0.0f;
    // Vertical hold: rolls from master x hold 0.65, 2.25 rows per frame per V Speed at 240.
    const double bar = fmax(v[V_BORDER], 0.0) * h;
    s[TV_VHOLD_ON] = (float)(master * v[VHOLD] > 0.65);
    const double roll = time * v[V_SPEED] * 2.25 * h / 240.0 + 0.618 * s[TV_SEED] * h +
                        v[V_RANDOM] * 0.05 * h * sin(time * v[V_FREQ] * 0.7);
    s[TV_VHOLD_OFFSET] = (float)fmod(fmod(roll, h + bar) + h + bar, h + bar);
    s[TV_VHOLD_BAR] = (float)bar;
    s[TV_VHOLD_DATA] = (float)v[V_DATA];
    s[TV_FF_AMP] = (float)fmax(v[FAST_FORWARD], 0.0);
    s[TV_FF_BANDS] = (float)v[FF_FREQ];
    s[TV_FF_SHIFT] = (float)v[FF_SHIFT];
    s[TV_FF_HEIGHT] = (float)v[FF_HEIGHT];
    s[TV_FF_PHASE] = (float)fmod(time * 0.02, 1.0);
    s[TV_DROP_AMP] = (float)(0.4 * fmax(v[DROPOUTS], 0.0));
    s[TV_DROP_CHANCE] = (float)v[DROP_ALWAYS];
    s[TV_DROP_LEN] = (float)(0.5 * v[DROP_LEN] * w);
    s[TV_DROP_GAP] = (float)(0.5 * v[DROP_GAP] * w);
    s[TV_DROP_FREQ] = (float)v[DROP_FREQ];
    s[TV_DROP_THRESHOLD] = (float)v[DROP_THRESHOLD];
    // Orthicon: the halo's gain 5.5 at Darks Width 0.2, 4.1 at 1.
    const double gain = fmax(4.1, 5.5 - 1.75 * (v[DARKS_WIDTH] - 0.2));
    s[TV_ORTH_AMP] = (float)(fmax(v[ORTHICON], 0.0) * gain);
    s[TV_ORTH_THRESHOLD] = (float)v[ORTH_THRESHOLD];
    const double f = v[COLOR_FREQ] / (0.5 * w), a = (v[COLOR_ANGLE] - 90.0) * M_PI / 180.0;
    s[TV_STRIPES_AMP] = (float)(master * v[STRIPES]);
    s[TV_STRIPES_FX] = (float)(f * cos(a));
    s[TV_STRIPES_FY] = (float)(f * sin(a));
    s[TV_STRIPES_PHASE] = (float)fmod(0.25 + v[ROLL_SPEED] * time / 24.0, 1.0);
    s[TV_HUE] = (float)v[HUE];
    s[TV_SATURATION] = (float)v[SATURATION];
    for (int c = 0; c < 3; ++c) {
        s[TV_LIGHTS_R + c] = (float)(v[SCALE_LIGHTS] * v[TINT_LIGHTS + c]);
        s[TV_DARKS_R + c] = (float)(v[OFFSET_DARKS] + v[TINT_DARKS + c]);
    }
    s[TV_BARS_AMP] = (float)(master * v[BARS]);
    s[TV_BARS_PERIOD] = (float)(0.5 * w);
    s[TV_BARS_FREQ] = (float)v[BAR_FREQ];
    s[TV_BARS_PHASE1] = (float)fmod(v[BAR_ROLL] * time * 0.03, 1.0);
    s[TV_BARS_PHASE2] = (float)fmod(v[BAR_ROLL] * time * 0.06, 1.0);
    s[TV_BARS_WIDTH] = (float)v[BAR1_WIDTH];
    s[TV_BARS_SHARP1] = (float)v[BAR_SHARP];
    s[TV_BARS_REL2] = (float)v[BAR2_REL];
    s[TV_BARS_SHARP2] = (float)v[BAR2_SHARP];
    s[TV_SCAN_AMP] = (float)v[SCANLINES];
    s[TV_SCAN_FREQ] = (float)(100.0 * v[SCAN_FREQ] / w);
    s[TV_VIG_AMP] = (float)v[VIGNETTE];
    s[TV_VIG_RADIUS] = (float)v[VIG_RADIUS];
    s[TV_VIG_SOFT] = (float)v[VIG_SOFT];
    s[TV_VIG_REL_H] = (float)fmax(v[VIG_REL_H], 0.01);
    s[TV_CELLS] = (float)(v[DOWNSAMPLE] != 0.0 && v[TV_PIXELS] < w);
    s[TV_FISH] = (float)v[FISH_EYE];
    s[TV_OFF] = (float)fmin(fmax(v[TURN_OFF], 0.0), 1.0);
    s[TV_OFF_FADE] = (float)(1.0 - fmin(fmax(v[FADE_TIME], 0.0), 1.0));
    s[TV_OFF_WIDTH] = (float)v[FLARE_WIDTH];
    s[TV_OFF_BRIGHT] = (float)v[FLARE_BRIGHT];
}

// One render: images and settings of the kernels.
typedef struct {
    MsImage *source, *settings, *picture, *bright, *narrow, *wide, *screen, *cells, *output;
    MsMask *mask;
    int orthicon, cells_on, narrow_radius, wide_radius, reach_x, reach_y, ghosts, octaves;
} Job;

static int run(MsApi api, void *user) {
    const Job *j = user;
    int status = ms_mask_run(j->mask, api);
    if (status == 0)
        status = MS_KERNEL(MS_K_TV_DAMAGE_SOURCE, api, MS_IMAGE("source", j->source),
                           MS_IMAGE("settings", j->settings), MS_INT("ghosts", j->ghosts),
                           MS_INT("octaves", j->octaves), MS_IMAGE("picture", j->picture));
    if (status == 0 && j->orthicon) {
        status = MS_KERNEL(MS_K_TV_DAMAGE_BRIGHT, api, MS_IMAGE("picture", j->picture),
                           MS_IMAGE("settings", j->settings), MS_IMAGE("bright", j->bright));
        if (status == 0)
            status = ms_rgba_blur(api, j->bright, j->narrow_radius, j->narrow_radius, j->narrow);
        if (status == 0)
            status = ms_rgba_blur(api, j->bright, j->wide_radius, j->wide_radius, j->wide);
    }
    if (status == 0)
        status = MS_KERNEL(MS_K_TV_DAMAGE_SCREEN, api, MS_IMAGE("picture", j->picture),
                           MS_IMAGE("narrow", j->orthicon ? j->narrow : j->picture),
                           MS_IMAGE("wide", j->orthicon ? j->wide : j->picture),
                           MS_IMAGE("settings", j->settings), MS_IMAGE("screen", j->screen));
    if (status == 0 && j->cells_on)
        status = MS_KERNEL(MS_K_PIXEL_SORT_DOWN, api, MS_IMAGE("source", j->screen),
                           MS_INT("reach_x", j->reach_x), MS_INT("reach_y", j->reach_y),
                           MS_IMAGE("image", j->cells));
    if (status == 0)
        status = MS_KERNEL(MS_K_TV_DAMAGE_FINAL, api, MS_IMAGE("screen", j->screen),
                           MS_IMAGE("cells", j->cells_on ? j->cells : j->screen),
                           MS_IMAGE("source", j->source), MS_IMAGE("mask_value", &j->mask->value),
                           MS_IMAGE("settings", j->settings), MS_IMAGE("output", j->output));
    return status;
}

static int render(const MsRender *r) {
    const double *v = r->values;
    const int w = r->width, h = r->height;
    float row[TV_FIELDS] = {0};
    settings(v, w, h, r->time, row);
    // TV pixels: square cells of w / TV Pixels.
    const int cells_on = row[TV_CELLS] > 0.5f;
    const int sw = cells_on ? (int)fmax(floor(v[TV_PIXELS] + 0.5), 1.0) : 1;
    const int sh = cells_on ? (int)fmin(fmax(floor((double)h * sw / w + 0.5), 1.0), h) : 1;
    // Orthicon: Gaussians of 2 and 13.5 x Darks Width pixels at 320 as three boxes (sigma
    // about total / 3 + 0.5).
    const double k = w / 320.0;
    const int orthicon = row[TV_ORTH_AMP] > 0.0f;
    const int narrow = (int)fmax(floor(3.0 * (2.0 * k - 0.5) + 0.5), 1.0);
    const int wide = (int)fmax(floor(3.0 * (13.5 * v[DARKS_WIDTH] * k - 0.5) + 0.5), narrow + 3);

    // The picture and the screen; the Orthicon's bright parts and their two blurs; the TV
    // pixels' cells; the mask value.
    const size_t image = (size_t)w * h * 4, ortho = orthicon ? image : 0;
    const size_t floats[] = {image,
                             image,
                             ortho,
                             ortho,
                             ortho,
                             cells_on ? (size_t)sw * sh * 4 : 0,
                             ms_mask_floats(r, MASK)};
    float *block = malloc(ms_parts_size(floats, ARRAY_SIZE(floats))), *part[ARRAY_SIZE(floats)];
    if (!block)
        return ms_out_of_memory(r);
    ms_parts(block, floats, ARRAY_SIZE(floats), part);
    MsImage source, settings_image, picture_image, screen_image, bright_image, narrow_image,
        wide_image, cells_image, output;
    ms_input_image(&source, r, SOURCE);
    ms_image(&settings_image, row, TV_FIELDS, TV_FIELDS, 1, 1);
    ms_image(&picture_image, part[0], (ptrdiff_t)w * 4, w, h, 4);
    ms_image(&screen_image, part[1], (ptrdiff_t)w * 4, w, h, 4);
    ms_image(&bright_image, part[2], (ptrdiff_t)w * 4, w, h, 4);
    ms_image(&narrow_image, part[3], (ptrdiff_t)w * 4, w, h, 4);
    ms_image(&wide_image, part[4], (ptrdiff_t)w * 4, w, h, 4);
    ms_image(&cells_image, part[5], (ptrdiff_t)sw * 4, sw, sh, 4);
    ms_output_image(&output, r);
    MsMask mask;
    ms_mask_init(&mask, r, MASK, SOURCE, &source, part[6], v[MASK_USE], v[INVERT_MASK],
                 v[BLUR_MASK]);
    Job j = {.source = &source,
             .mask = &mask,
             .settings = &settings_image,
             .picture = &picture_image,
             .bright = &bright_image,
             .narrow = &narrow_image,
             .wide = &wide_image,
             .screen = &screen_image,
             .cells = &cells_image,
             .output = &output,
             .orthicon = orthicon,
             .cells_on = cells_on,
             .narrow_radius = narrow,
             .wide_radius = wide,
             .reach_x = (int)ceil((double)w / sw) + 1,
             .reach_y = (int)ceil((double)h / sh) + 1,
             .ghosts = (int)row[TV_GHOSTS],
             .octaves = (int)row[TV_HHOLD_OCTAVES]};
    // Inputs uploaded to the GPU: the Source, the Mask or the one mask value, the settings.
    MsImage *const inputs[] = {&source, mask.input, &settings_image};
    MsImage *const between[] = {&picture_image,
                                &screen_image,
                                ms_mask_between(&mask),
                                orthicon ? &bright_image : NULL,
                                orthicon ? &narrow_image : NULL,
                                orthicon ? &wide_image : NULL,
                                cells_on ? &cells_image : NULL};
    const int ok = ms_run(run, &j, inputs, ARRAY_SIZE(inputs), between, ARRAY_SIZE(between),
                          &output, r->info, r->err, r->err_size);
    free(block);
    return ok;
}

const MsEffect ms_tvdamage_effect = {
    .id = MS_ID("TVDamage"),
    .label = "MS_TVDamage",
    .description =
        "A damaged TV picture: static, interference, ghosts, rolling and sliding holds, bars, "
        "stripes, scanlines, vignette, colour, fish eye and turning off.",
    MS_CLIPS(clips),
    MS_PARAMS(params),
    .render = render,
    .frame_varying = 1,
    .presets = "tvdamage",
};
