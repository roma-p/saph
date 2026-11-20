// MS_LensFlare: one lens flare, its elements (Glare's: glows, rays, sectors, rings) placed
// along the axis from the Pivot through the Hotspot (src/halide/lensflare_generator.cpp).
//   - the lens (a JSON text, glare_style.c in lens mode): each element at pivot + center x
//     (hotspot - pivot), sized by Scale Widths (x 1.375, Glare's size unit), scaled by its
//     distance to the pivot as it asks, its brightness b -> b^(1 / Gamma); the elements at
//     the hotspot (center 1) by the Hotspot settings, the others by the Other ones, the rays
//     also by the Rays ones
//   - the Center and Edge Triggers: the whole flare brighter and wider when the hotspot is
//     near the pivot, or near the frame's edges
//   - the Occlusion input: the flare hidden by its alpha (or luma) around the hotspot; the
//     Diffraction Glow: its edges near the hotspot glow
//   - the Atmosphere: the elements (but those that ignore it) x a noise over the frame,
//     changing over time (MillSaph's own noise)
//   - the finish: Brightness x Color, Hue Shift, Saturation, Blur Flare; screened over (or
//     added to) the Source x Bg Brightness, or alone; Tint Bg Whites, Affect Alpha
// The Preset menu picks one of MillSaph's own lenses (presets/lensflare) instead of the Lens
// text. Scale Widths, Blur Flare and the triggers' sizes are "normalised" values, so hosts show
// them in pixels; Hotspot and Pivot are positions.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/defines.h"
#include "common/effect.h"
#include "common/mutex.h"
#include "common/process.h"
#include "effects/atmosphere.h"
#include "effects/glare_style.h"
#include "engine/engine.h"
#include "lensflare_presets.h" // (generated from presets/lensflare)

// Inputs: clip, label in the host, value where there is no image, optional, mask.
static const MsClip clips[] = {
    {"Source", "Source", 0.0f, 0, 0},
    {"Occlusion", "Occlusion", 0.0f, 1, 0},
};
enum { SOURCE, OCCLUSION };

static const char *const combine_entries[] = {"Screen", "Add", "Flare Only"};
static const char *const occlusion_entries[] = {"None", "Luma", "Alpha"};

// MillSaph's default lens: a warm glow and star at the hotspot, a streak, ghosts along the
// axis (discs, rings, a hexagon), a wide faint ring past the pivot, and the lens's dirt lit
// around the hotspot (MillSaph's own texture).
static const char default_lens[] =
    "{\"elements\": [\n"
    " {\"type\": \"blob\", \"name\": \"Glow\", \"center\": 1, \"size\": 1.2, \"brightness\": 0.35,"
    " \"colors\": [1, 0.85, 0.7]},\n"
    " {\"type\": \"blob\", \"name\": \"Core\", \"center\": 1, \"size\": 0.25, \"brightness\": 0.9,"
    " \"colors\": [1, 0.97, 0.92]},\n"
    " {\"type\": \"rays\", \"name\": \"Star\", \"center\": 1, \"size\": 0.9, \"brightness\": 0.2,"
    " \"ray_number\": 12, \"ray_width\": 0.015, \"color\": [1, 0.92, 0.85]},\n"
    " {\"type\": \"rays\", \"name\": \"Streak\", \"center\": 1, \"size\": 2.5, \"brightness\": 0.12,"
    " \"ray_number\": 2, \"ray_width\": 0.006, \"color\": [0.75, 0.85, 1]},\n"
    " {\"type\": \"spot\", \"name\": \"Ghost 1\", \"center\": 0.55, \"size\": 0.12,"
    " \"brightness\": 0.1, \"radii\": [0, 0.8, 1],"
    " \"colors\": [0.6, 0.8, 1, 0.7, 0.9, 1, 0.4, 0.6, 1]},\n"
    " {\"type\": \"ring\", \"name\": \"Ghost Ring\", \"center\": 0.25, \"size\": 0.3,"
    " \"brightness\": 0.06, \"radii\": [0.8, 0.95, 1], \"colors\": [0, 0, 0, 1, 0.8, 0.5, 0, 0, 0]},\n"
    " {\"type\": \"blob\", \"name\": \"Ghost 2\", \"center\": -0.3, \"size\": 0.35,"
    " \"brightness\": 0.06, \"colors\": [0.6, 1, 0.7]},\n"
    " {\"type\": \"fancy_spot\", \"name\": \"Hexagon\", \"center\": -0.7, \"size\": 0.22,"
    " \"brightness\": 0.08, \"n_sides\": 6, \"roundness\": 0.15, \"blur_gradient\": 0.2,"
    " \"radii\": [0, 1], \"colors\": [0.5, 0.6, 1, 0.8, 0.7, 1]},\n"
    " {\"type\": \"blob\", \"name\": \"Ghost 3\", \"center\": -1.1, \"size\": 0.18,"
    " \"brightness\": 0.07, \"colors\": [1, 0.6, 0.4], \"scale_brightness_by_dist\": 0.5},\n"
    " {\"type\": \"ring\", \"name\": \"Halo\", \"center\": -1.5, \"size\": 0.7, \"brightness\": 0.03,"
    " \"radii\": [0.85, 0.95, 1], \"colors\": [0, 0, 0, 0.8, 0.9, 1, 0, 0, 0],"
    " \"offset_stretch_by_dist\": 0.3},\n"
    " {\"type\": \"blob\", \"name\": \"Lens Dirt\", \"center\": 1, \"size\": 1.0,"
    " \"brightness\": 0.18, \"colors\": [1, 0.95, 0.9], \"image\": {\"generate\": \"dirt\","
    " \"seed\": 1}, \"image_absolute_position_p\": 1, \"image_absolute_size_p\": 1}\n"
    "]}\n";

enum {
    PRESET,
    EDIT_LENS,
    LENS,
    SCALE_WIDTHS,
    REL_HEIGHTS,
    BLUR_FLARE,
    HOTSPOT,
    HOTSPOT_Y,
    PIVOT,
    PIVOT_Y,
    BRIGHTNESS,
    COLOR,
    COLOR_G,
    COLOR_B,
    GAMMA,
    SATURATION,
    HUE_SHIFT,
    RAYS_BRIGHTNESS,
    RAYS_ROTATE,
    RAYS_NUM_SCALE,
    RAYS_LENGTH,
    RAYS_THICKNESS,
    HOTSPOT_BRIGHT,
    HOTSPOT_COLOR,
    HOTSPOT_COLOR_G,
    HOTSPOT_COLOR_B,
    OTHER_BRIGHTNESS,
    OTHER_COLOR,
    OTHER_COLOR_G,
    OTHER_COLOR_B,
    OTHER_WIDTH,
    BG_BRIGHTNESS,
    COMBINE,
    TINT_BG_WHITES,
    AFFECT_ALPHA,
    EDGE_WIDTH,
    EDGE_FALLOFF,
    EDGE_SCALE_BRIGHTNESS,
    EDGE_SCALE_WIDTHS,
    CENTER_RADIUS,
    CENTER_FALLOFF,
    CENTER_SCALE_BRIGHTNESS,
    CENTER_SCALE_WIDTHS,
    OCCLUSION_SOFTNESS,
    OCCLUSION_FROM,
    INVERT_OCCLUSION,
    OCCLUSION_SCALE_BRIGHTNESS,
    OCCLUSION_SCALE_WIDTHS,
    DIFFRACTION_GLOW,
    GLOW_WIDTH,
    GLOW_RADIUS,
    GLOW_COLOR,
    GLOW_COLOR_G,
    GLOW_COLOR_B,
    ATMOSPHERE_AMP,
    ATMOSPHERE_FREQ,
    ATMOSPHERE_DETAIL,
    ATMOSPHERE_SEED,
    ATMOSPHERE_SPEED,
    FLICKER_AMP,
    FLICKER_SPEED,
    FLICKER_RANDOMNESS,
    ENABLE_MOTION_BLUR,
    SHUTTER_ANGLE
};
#define FALLOFF(id, n, l, h)                                                                       \
    [id] = {.type = MS_PARAM_DOUBLE,                                                               \
            .name = (n),                                                                           \
            .label = (l),                                                                          \
            .hint = (h),                                                                           \
            .def = 1.0,                                                                            \
            .min = 0.01,                                                                           \
            .max = MS_UNBOUNDED,                                                                   \
            .display_min = 0.01,                                                                   \
            .display_max = 5.0}
static int edit_lens(MsHost *host, char *err, size_t err_size); // (the Edit Lens button)

static const MsParam params[] = {
    [PRESET] = {.type = MS_PARAM_CHOICE,
                .name = "preset",
                .label = "Preset",
                .hint = "The lens: the Lens text, or one of MillSaph's presets.",
                .entries = ms_lensflare_preset_menu,
                .entry_count = ARRAY_SIZE(ms_lensflare_preset_menu)},
    [EDIT_LENS] = {.type = MS_PARAM_BUTTON,
                   .name = "edit_lens",
                   .label = "Edit Lens",
                   .hint = "Opens MillSaph's lens editor on the lens in use; what it saves "
                           "becomes the Lens text.",
                   .press = edit_lens},
    [LENS] = {.type = MS_PARAM_STRING,
              .name = "lens_",
              .label = "Lens",
              .hint = "The flare's elements, as JSON text: {\"elements\": [...]}, each a blob, "
                      "spot, ring, rays, polar_flare or fancy_spot at `center` along the axis "
                      "(1: the hotspot, 0: the pivot).",
              .def_text = default_lens},
    [SCALE_WIDTHS] = {.type = MS_PARAM_WIDTH,
                      .name = "scale_widths",
                      .label = "Scale Widths",
                      .hint = "The size of the flare's elements.",
                      .def = 0.21,
                      .min = 0.0,
                      .max = MS_UNBOUNDED,
                      .display_max = 1.0},
    [REL_HEIGHTS] = {.type = MS_PARAM_DOUBLE,
                     .name = "rel_heights",
                     .label = "Rel Heights",
                     .hint = "The elements' height relative to their width.",
                     .def = 1.0,
                     .min = 0.1,
                     .max = 10.0,
                     .display_min = 0.1,
                     .display_max = 10.0},
    [BLUR_FLARE] = {.type = MS_PARAM_WIDTH,
                    .name = "blur_flare",
                    .label = "Blur Flare",
                    .hint = "Blurs the flare.",
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 0.1},
    [HOTSPOT] = {.type = MS_PARAM_POINT,
                 .name = "hotspot",
                 .label = "Hotspot",
                 .hint = "Where the light is: the flare's elements lie on the line from the "
                         "Pivot through it.",
                 .def = 0.2778},
    [HOTSPOT_Y] = {.type = MS_PARAM_POINT_Y, .name = "hotspot_y", .def = 0.6173},
    [PIVOT] = {.type = MS_PARAM_POINT,
               .name = "pivot",
               .label = "Pivot",
               .hint = "The lens's centre: the flare's axis goes through it.",
               .def = 0.5},
    [PIVOT_Y] = {.type = MS_PARAM_POINT_Y, .name = "pivot_y", .def = 0.5},
    MS_RELATIVE(BRIGHTNESS, "brightness", "Brightness", "Scales the brightness of the flare.", 1.0),
    MS_COLOUR(COLOR, "color", "Color", "Scales the color of the flare."),
    [GAMMA] = {.type = MS_PARAM_DOUBLE,
               .name = "gamma",
               .label = "Gamma",
               .hint = "Raises the brightness of the faint elements more than the bright ones.",
               .def = 1.0,
               .min = 0.01,
               .max = MS_UNBOUNDED,
               .display_min = 1.0,
               .display_max = 3.0},
    [SATURATION] = {.type = MS_PARAM_DOUBLE,
                    .name = "saturation",
                    .label = "Saturation",
                    .hint = "The flare's saturation (0: grey).",
                    .def = 1.0,
                    .min = 0.0,
                    .max = MS_UNBOUNDED,
                    .display_max = 2.0},
    [HUE_SHIFT] = {.type = MS_PARAM_DOUBLE,
                   .name = "hue_shift",
                   .label = "Hue Shift",
                   .hint = "Turns the hue of the flare, in turns of the color wheel.",
                   .min = -MS_UNBOUNDED,
                   .max = MS_UNBOUNDED,
                   .display_min = -1.0,
                   .display_max = 1.0},
    MS_RELATIVE(RAYS_BRIGHTNESS, "rays_brightness", "Rays Brightness",
             "Scales the brightness of the rays.", 1.0),
    [RAYS_ROTATE] = {.type = MS_PARAM_ANGLE,
                     .name = "rays_rotate",
                     .label = "Rays Rotate",
                     .hint = "Turns the rays (degrees, counter-clockwise).",
                     .min = -MS_UNBOUNDED,
                     .max = MS_UNBOUNDED,
                     .display_min = -360.0,
                     .display_max = 360.0},
    MS_RELATIVE(RAYS_NUM_SCALE, "rays_num_scale", "Rays Num Scale", "Scales the number of rays.", 1.0),
    MS_RELATIVE(RAYS_LENGTH, "rays_length", "Rays Length", "Scales the length of the rays.", 1.0),
    MS_RELATIVE(RAYS_THICKNESS, "rays_thickness", "Rays Thickness", "Scales the width of the rays.",
             1.0),
    MS_RELATIVE(HOTSPOT_BRIGHT, "hotspot_bright", "Hotspot Bright",
             "Scales the brightness of the elements at the hotspot.", 1.0),
    MS_COLOUR(HOTSPOT_COLOR, "hotspot_color", "Hotspot Color",
           "Scales the color of the elements at the hotspot."),
    MS_RELATIVE(OTHER_BRIGHTNESS, "other_brightness", "Other Brightness",
             "Scales the brightness of the elements away from the hotspot.", 1.0),
    MS_COLOUR(OTHER_COLOR, "other_color", "Other Color",
           "Scales the color of the elements away from the hotspot."),
    MS_RELATIVE(OTHER_WIDTH, "other_width", "Other Width",
             "Scales the size of the elements away from the hotspot.", 1.0),
    MS_FRACTION(BG_BRIGHTNESS, "bg_brightness", "Bg Brightness", "Scales the Source under the flare.",
             1.0),
    [COMBINE] = {.type = MS_PARAM_CHOICE,
                 .name = "combine",
                 .label = "Combine",
                 .hint = "How the flare goes over the Source: screened, added, or the flare "
                         "alone.",
                 .entries = combine_entries,
                 .entry_count = ARRAY_SIZE(combine_entries)},
    [TINT_BG_WHITES] = {.type = MS_PARAM_BOOL,
                        .name = "tint_bg_whites",
                        .label = "Tint Bg Whites",
                        .hint = "Tints the Source by the flare's color where the flare is."},
    MS_FRACTION(AFFECT_ALPHA, "affect_alpha", "Affect Alpha", "Adds the flare to the alpha.", 0.0),
    MS_DISTANCE(EDGE_WIDTH, "edge_width", "Edge Width",
             "The Edge Triggers act when the hotspot is closer than this to the frame's edges.",
             0.15),
    [EDGE_FALLOFF] = {.type = MS_PARAM_DOUBLE,
                      .name = "edge_falloff",
                      .label = "Edge Falloff",
                      .hint = "How fast the Edge Triggers fade away from the edges.",
                      .def = 2.0,
                      .min = 0.01,
                      .max = MS_UNBOUNDED,
                      .display_min = 0.01,
                      .display_max = 5.0},
    MS_RELATIVE(EDGE_SCALE_BRIGHTNESS, "edge_scale_brightness", "Edge Scale Brightness",
             "Scales the flare's brightness at the frame's edges.", 1.5),
    MS_RELATIVE(EDGE_SCALE_WIDTHS, "edge_scale_widths", "Edge Scale Widths",
             "Scales the flare's size at the frame's edges.", 1.2),
    MS_DISTANCE(CENTER_RADIUS, "center_radius", "Center Radius",
             "The Center Triggers act when the hotspot is closer than this to the pivot.", 0.15),
    FALLOFF(CENTER_FALLOFF, "center_falloff", "Center Falloff",
            "How fast the Center Triggers fade away from the pivot."),
    MS_RELATIVE(CENTER_SCALE_BRIGHTNESS, "center_scale_brightness", "Center Scale Brightness",
             "Scales the flare's brightness near the pivot.", 1.5),
    MS_RELATIVE(CENTER_SCALE_WIDTHS, "center_scale_widths", "Center Scale Widths",
             "Scales the flare's size near the pivot.", 1.2),
    [OCCLUSION_SOFTNESS] = {.type = MS_PARAM_WIDTH,
                            .name = "occlusion_softness",
                            .label = "Occlusion Softness",
                            .hint = "The size of the area around the hotspot the Occlusion "
                                    "input covers to hide the flare.",
                            .def = 0.0028,
                            .min = 0.0,
                            .max = MS_UNBOUNDED,
                            .display_max = 0.25},
    [OCCLUSION_FROM] = {.type = MS_PARAM_CHOICE,
                        .name = "occlusion_from",
                        .label = "Occlusion From",
                        .hint = "What of the Occlusion input hides the flare: nothing, its luma "
                                "or its alpha.",
                        .def = 2,
                        .entries = occlusion_entries,
                        .entry_count = ARRAY_SIZE(occlusion_entries)},
    [INVERT_OCCLUSION] = {.type = MS_PARAM_BOOL,
                          .name = "invert_occlusion",
                          .label = "Invert Occlusion",
                          .hint = "The black areas of the Occlusion input hide the flare."},
    MS_RELATIVE(OCCLUSION_SCALE_BRIGHTNESS, "occlusion_scale_brightness",
             "Occlusion Scale Brightness",
             "Scales the flare's brightness as the Occlusion input hides it.", 1.0),
    MS_RELATIVE(OCCLUSION_SCALE_WIDTHS, "occlusion_scale_widths", "Occlusion Scale Widths",
             "Scales the flare's size as the Occlusion input hides it.", 1.0),
    MS_RELATIVE(DIFFRACTION_GLOW, "diffraction_glow", "Diffraction Glow",
             "The glow of the Occlusion input's edges near the hotspot.", 0.0),
    MS_DISTANCE(GLOW_WIDTH, "glow_width", "Glow Width", "How far the Diffraction Glow spreads.",
             0.05),
    MS_DISTANCE(GLOW_RADIUS, "glow_radius", "Glow Radius",
             "How far from the hotspot the Occlusion input's edges glow.", 0.025),
    MS_COLOUR(GLOW_COLOR, "glow_color", "Glow Color", "The color of the Diffraction Glow."),
    [ATMOSPHERE_AMP] = {.type = MS_PARAM_DOUBLE,
                        .name = "atmosphere_amp",
                        .label = "Atmosphere Amp",
                        .hint = "How much a cloudy noise over the frame varies the flare's "
                                "brightness (0: none).",
                        .min = 0.0,
                        .max = MS_UNBOUNDED,
                        .display_max = 5.0},
    [ATMOSPHERE_FREQ] = {.type = MS_PARAM_DOUBLE,
                         .name = "atmosphere_freq",
                         .label = "Atmosphere Freq",
                         .hint = "The size of the Atmosphere's clouds (higher: smaller).",
                         .def = 1.0,
                         .min = 0.01,
                         .max = MS_UNBOUNDED,
                         .display_min = 0.1,
                         .display_max = 20.0},
    MS_FRACTION(ATMOSPHERE_DETAIL, "atmosphere_detail", "Atmosphere Detail",
             "How much fine detail the Atmosphere has.", 0.6),
    [ATMOSPHERE_SEED] = {.type = MS_PARAM_DOUBLE,
                         .name = "atmosphere_seed",
                         .label = "Atmosphere Seed",
                         .hint = "Another Atmosphere for each value.",
                         .def = 0.123,
                         .min = -MS_UNBOUNDED,
                         .max = MS_UNBOUNDED,
                         .display_max = 5.0},
    [ATMOSPHERE_SPEED] = {.type = MS_PARAM_DOUBLE,
                          .name = "atmosphere_speed",
                          .label = "Atmosphere Speed",
                          .hint = "How fast the Atmosphere changes over time.",
                          .def = 1.0,
                          .min = -MS_UNBOUNDED,
                          .max = MS_UNBOUNDED,
                          .display_min = -5.0,
                          .display_max = 5.0},
    [FLICKER_AMP] = {.type = MS_PARAM_DOUBLE,
                     .name = "flicker_amp",
                     .label = "Flicker Amp",
                     .hint = "How much the flare's brightness flickers over time (0: none).",
                     .min = 0.0,
                     .max = MS_UNBOUNDED,
                     .display_max = 2.0},
    [FLICKER_SPEED] = {.type = MS_PARAM_DOUBLE,
                       .name = "flicker_speed",
                       .label = "Flicker Speed",
                       .hint = "How fast it flickers (0: still).",
                       .def = 1.0,
                       .min = 0.0,
                       .max = MS_UNBOUNDED,
                       .display_max = 5.0},
    MS_FRACTION(FLICKER_RANDOMNESS, "flicker_randomness", "Flicker Randomness",
             "How rarely it flickers: 0 all the time, 1 never; between, only the strongest "
             "flickers.",
             0.6),
    [ENABLE_MOTION_BLUR] = {.type = MS_PARAM_BOOL,
                            .name = "enable_motion_blur",
                            .label = "Enable Motion Blur",
                            .hint = "Blurs the flare along its elements' motion when the hotspot "
                                    "or the pivot moves (animated)."},
    [SHUTTER_ANGLE] = {.type = MS_PARAM_DOUBLE,
                       .name = "shutter_angle",
                       .label = "Shutter Angle",
                       .hint = "How long the shutter is open, in degrees of a frame (360: a "
                               "whole frame), centred on the frame.",
                       .def = 180.0,
                       .min = 0.0,
                       .max = 360.0,
                       .display_max = 360.0},
};

// The Motion Blur's samples: about one every this many pixels of travel, at most this many.
#define MOTION_SPACING 3.0
#define MOTION_SAMPLES (GLARE_MAX_ROWS / GLARE_MAX_ELEMENTS)

// One render: images and settings of the kernels.
typedef struct {
    MsImage *source, *occluder, *rows, *noise_rows, *texels, *visible, *flare, *glowed, *output;
    int from, invert, count, blur, combine, tint;
    int sizes[GLARE_DRAW_LISTS]; // the rows drawn, by how (GLARE_DRAW_*): list k's size
    int spot[2], square; // the occlusion's square: its centre pixel, its half side
    float gain[3], hue, saturation, blur_frac, bg, affect_alpha;
    float occluded_bright, occluded_width; // Occlusion Scale Brightness, Widths, minus 1
    float flicker_amp, flicker_z, flicker_dead, flicker_scale; // (ms_lensflare_finish)
    // the Atmosphere (draw)
    float amp, freq, persistence, z;
    int octaves;
    // the Diffraction Glow (glowed: NULL without)
    float glow_at[2], reach, edge_frac, glow_frac, glow_gain[3];
    int edge, glow_blur;
} Job;

// The samples of each row's profile in ms_lensflare_draw's table: 16384 (a sample every 0.25
// pixel up to 4096 pixels from an element's centre), fewer with many rows (Motion Blur's), the
// table at most 8M floats (32 MB).
static int profile_samples(int rows) {
    const int budget = (8 << 20) / (3 * (rows > 1 ? rows : 1));
    return budget > 16384 ? 16384 : budget < 1024 ? 1024 : budget;
}

// The occlusion at the hotspot, the elements drawn, the finish; on `api`.
static int run(MsApi api, void *user) {
    const Job *j = user;
    int status = MS_KERNEL(MS_K_LENSFLARE_OCCLUSION, api, MS_IMAGE("occluder", j->occluder),
                           MS_INT("from", j->from), MS_INT("invert", j->invert),
                           MS_INT("hotspot_x", j->spot[0]), MS_INT("hotspot_y", j->spot[1]),
                           MS_INT("reach", j->square),
                           MS_IMAGE("visible", j->visible));
    if (status == 0)
        status = MS_KERNEL(MS_K_LENSFLARE_DRAW, api, MS_IMAGE("rows", j->rows),
                           MS_IMAGE("noise_rows", j->noise_rows), MS_INT("plain", j->sizes[0]),
                           MS_INT("shadowed", j->sizes[1]), MS_INT("special", j->sizes[2]),
                           MS_INT("special_shadowed", j->sizes[3]),
                           MS_IMAGE("texels", j->texels),
                           MS_IMAGE("visible", j->visible),
                           MS_INT("count", j->count > 0 ? j->count : 1),
                           MS_INT("samples", profile_samples(j->count)),
                           MS_FLOAT("amp", j->amp), MS_FLOAT("freq", j->freq),
                           MS_FLOAT("persistence", j->persistence), MS_FLOAT("z", j->z),
                           MS_INT("octaves", j->octaves),
                           MS_FLOAT("occluded_bright", j->occluded_bright),
                           MS_FLOAT("occluded_width", j->occluded_width),
                           MS_IMAGE("flare", j->flare));
    if (status == 0 && j->glowed)
        status = MS_KERNEL(
            MS_K_LENSFLARE_GLOW, api, MS_IMAGE("flare", j->flare),
            MS_IMAGE("occluder", j->occluder), MS_INT("from", j->from),
            MS_INT("invert", j->invert), MS_IMAGE("visible", j->visible),
            MS_FLOAT("hotspot_x", j->glow_at[0]), MS_FLOAT("hotspot_y", j->glow_at[1]),
            MS_INT("edge", j->edge), MS_FLOAT("edge_frac", j->edge_frac),
            MS_FLOAT("reach", j->reach), MS_INT("blur", j->glow_blur),
            MS_FLOAT("blur_frac", j->glow_frac), MS_FLOAT("gain_r", j->glow_gain[0]),
            MS_FLOAT("gain_g", j->glow_gain[1]), MS_FLOAT("gain_b", j->glow_gain[2]),
            MS_IMAGE("output", j->glowed));
    if (status != 0)
        return status;
    return MS_KERNEL(MS_K_LENSFLARE_FINISH, api,
                     MS_IMAGE("flare", j->glowed ? j->glowed : j->flare),
                     MS_IMAGE("source", j->source), MS_FLOAT("gain_r", j->gain[0]),
                     MS_FLOAT("gain_g", j->gain[1]), MS_FLOAT("gain_b", j->gain[2]),
                     MS_FLOAT("hue", j->hue), MS_FLOAT("saturation", j->saturation),
                     MS_INT("blur", j->blur), MS_FLOAT("blur_frac", j->blur_frac),
                     MS_FLOAT("bg", j->bg), MS_INT("combine", j->combine), MS_INT("tint", j->tint),
                     MS_FLOAT("affect_alpha", j->affect_alpha),
                     MS_FLOAT("flicker_amp", j->flicker_amp), MS_FLOAT("flicker_z", j->flicker_z),
                     MS_FLOAT("flicker_dead", j->flicker_dead),
                     MS_FLOAT("flicker_scale", j->flicker_scale), MS_IMAGE("output", j->output));
}

// A width in pixels: whole pixels and the fraction past them.
static void split(double v, int *whole, float *frac) {
    v = v > 0 ? (v < 1e7 ? v : 1e7) : 0;
    *whole = (int)floor(v);
    *frac = (float)(v - *whole);
}

// The 3-box blur's total radius for a gaussian of sigma s (each box of radius r has the
// variance r (r + 1) / 3).
static double box_total(double s) { return 1.5 * (sqrt(1.0 + 4.0 * fmax(s, 0.0) * s) - 1.0); }

// MillSaph's textures generated by the last renders (the same settings: the same texels),
// shared by the renders (a lock), the oldest replaced.
#define TEXTURE_CACHE 8
static struct {
    MsLensTexture key; // (its start unused)
    float *texels;
    unsigned long long used;
} texture_cache[TEXTURE_CACHE];
static unsigned long long texture_clock;
static MsMutex texture_lock = MS_MUTEX_INIT;

static int same_texture(const MsLensTexture *a, const MsLensTexture *b) {
    return a->kind == b->kind && a->seed == b->seed && a->amount == b->amount &&
           a->scale == b->scale && a->detail == b->detail && a->sides == b->sides &&
           a->width == b->width && a->height == b->height && a->mean == b->mean &&
           a->edge_x == b->edge_x && a->edge_y == b->edge_y;
}

// Texture t into `out` (its width x height x 3 floats): from the cache, or generated (on the
// CPU) and kept. Returns Halide's status (0: success).
static int texture(const MsLensTexture *t, float *out) {
    const size_t floats = (size_t)t->width * t->height * 3;
    ms_mutex_lock(&texture_lock);
    for (int i = 0; i < TEXTURE_CACHE; ++i)
        if (texture_cache[i].texels && same_texture(&texture_cache[i].key, t)) {
            memcpy(out, texture_cache[i].texels, floats * sizeof(float));
            texture_cache[i].used = ++texture_clock;
            ms_mutex_unlock(&texture_lock);
            return 0;
        }
    ms_mutex_unlock(&texture_lock);
    MsImage image;
    ms_image(&image, out, (ptrdiff_t)t->width * 3, t->width, t->height, 3);
    const int status = MS_KERNEL(MS_K_LENSFLARE_TEXTURE, MS_API_CPU, MS_INT("kind", t->kind),
                                 MS_FLOAT("seed", t->seed), MS_FLOAT("amount", t->amount),
                                 MS_FLOAT("scale", t->scale), MS_FLOAT("detail", t->detail),
                                 MS_INT("sides", t->sides), MS_FLOAT("edge_x", t->edge_x),
                                 MS_FLOAT("edge_y", t->edge_y), MS_IMAGE("texture", &image));
    if (status == 0 && t->mean > 0.0) { // scaled to the mean wanted (where the lens shows it:
        double sum = 0.0; // in the disc inscribed in the texture), the image's it stands for
        size_t count = 0;
        const int side = t->width < t->height ? t->width : t->height;
        for (int y = 0; y < t->height; ++y)
            for (int x = 0; x < t->width; ++x) {
                const double dx = (x + 0.5 - 0.5 * t->width) / side;
                const double dy = (y + 0.5 - 0.5 * t->height) / side;
                if (dx * dx + dy * dy > 0.25)
                    continue;
                const float *p = out + ((size_t)y * t->width + x) * 3;
                sum += (double)p[0] + p[1] + p[2];
                count += 3;
            }
        const float gain = sum > 0.0 ? (float)(t->mean * (double)count / sum) : 1.0f;
        for (size_t i = 0; i < floats; ++i)
            out[i] *= gain;
    }
    float *copy = status == 0 ? malloc(floats * sizeof(float)) : NULL;
    if (!copy)
        return status;
    memcpy(copy, out, floats * sizeof(float));
    ms_mutex_lock(&texture_lock);
    int oldest = 0;
    for (int i = 1; i < TEXTURE_CACHE; ++i)
        if (texture_cache[i].used < texture_cache[oldest].used)
            oldest = i;
    free(texture_cache[oldest].texels);
    texture_cache[oldest].key = *t;
    texture_cache[oldest].texels = copy;
    texture_cache[oldest].used = ++texture_clock;
    ms_mutex_unlock(&texture_lock);
    return 0;
}

// Frees the textures kept (the plugin is unloaded).
static void release(void) {
    ms_mutex_lock(&texture_lock);
    for (int i = 0; i < TEXTURE_CACHE; ++i) {
        free(texture_cache[i].texels);
        texture_cache[i].texels = NULL;
    }
    ms_mutex_unlock(&texture_lock);
}

// Edit Lens: the lens in use (the preset, else the Lens text) into a file, MillSaph's lens
// editor (millsaph_lens_editor, next to the plugin) run on it and waited for; when it saves
// (exit code 0), its lens becomes the Lens text and the Preset menu goes back to it.
static int edit_lens(MsHost *host, char *err, size_t err_size) {
    char dir[1024], editor[1200], in[1024], out[1024];
#ifdef _WIN32
    const char *exe = "millsaph_lens_editor.exe";
#else
    const char *exe = "millsaph_lens_editor";
#endif
    if (!ms_module_dir(dir, sizeof dir)) {
        snprintf(err, err_size, "cannot find where the plugin is");
        return 0;
    }
    snprintf(editor, sizeof editor, "%s%s", dir, exe);
    if (!ms_temp_file(".json", in, sizeof in) || !ms_temp_file(".json", out, sizeof out)) {
        snprintf(err, err_size, "cannot make a temporary file");
        return 0;
    }
    const int preset = (int)host->get_value(host, params[PRESET].name);
    const char *text = host->get_text(host, params[LENS].name);
    const char *lens = preset > 0 && preset <= MS_LENSFLARE_PRESETS
                           ? ms_lensflare_preset_texts[preset - 1]
                       : text ? text
                              : params[LENS].def_text;
    FILE *f = fopen(in, "wb");
    int ok = f && fputs(lens, f) >= 0;
    if (f)
        fclose(f);
    const char *argv[] = {editor, "--in", in, "--out", out, NULL};
    const int code = ok ? ms_run_program(argv) : -1;
    char *edited = NULL;
    if (code == 0) { // saved: the file's text
        f = fopen(out, "rb");
        long size = -1;
        if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0)
            edited = malloc((size_t)size + 1);
        if (edited && fread(edited, 1, (size_t)size, f) == (size_t)size)
            edited[size] = 0;
        else {
            free(edited);
            edited = NULL;
        }
        if (f)
            fclose(f);
        ok = edited && host->set_text(host, params[LENS].name, edited) &&
             host->set_value(host, params[PRESET].name, 0.0);
    }
    free(edited);
    remove(in);
    remove(out);
    if (!ok || code < 0)
        snprintf(err, err_size, code < 0 ? "cannot run the lens editor (%s)"
                                         : "cannot set the edited lens", editor);
    return ok && code >= 0;
}

// A trigger's weight at distance d from where it acts most (radius: how far it reaches).
static double trigger(double d, double radius, double falloff) {
    const double t = 1.0 - fmax(d, 0.0) / (1.034 * radius);
    return radius > 0 && t > 0 ? 0.977 * pow(t, falloff) : 0.0;
}

// The rows' settings from the values `v` (at the frame, or at a time of the motion blur) on
// a frame of fw x fh pixels.
static void lens_globals(const double *v, double fw, double fh, MsGlareGlobals *g) {
    // The triggers: by the hotspot's distance to the pivot, and to the frame's nearest edge.
    // (the hotspot and the pivot one pixel higher than their values, as the reference draws
    // them)
    const double hx = v[HOTSPOT], hy = v[HOTSPOT_Y] + 1.0, px = v[PIVOT], py = v[PIVOT_Y] + 1.0;
    // The triggers' distances: on the frame squeezed to a square (y x width / height).
    const double aspect = fh > 0 ? fw / fh : 1.0;
    const double to_pivot = hypot(hx - px, (hy - py) * aspect);
    // (outside the frame: the distance to its edge)
    const double to_edge = fabs(fmin(fmin(hx, fw - hx), fmin(hy, fh - hy) * aspect));
    const double wc = trigger(to_pivot, v[CENTER_RADIUS], v[CENTER_FALLOFF]);
    const double we = trigger(to_edge, v[EDGE_WIDTH], v[EDGE_FALLOFF]);
    *g = (MsGlareGlobals){
        .size = 1.375 * fmax(v[SCALE_WIDTHS], 0.0),
        .rel_height = v[REL_HEIGHTS],
        .rays_num_scale = v[RAYS_NUM_SCALE],
        .rays_length = v[RAYS_LENGTH],
        .rays_thickness = v[RAYS_THICKNESS],
        .lens = 1,
        .hotspot = {hx, hy},
        .pivot = {px, py},
        .unit = fmax(fw / 4.0, 1e-6),
        .frame_width = fw,
        .frame_height = fh,
        .gamma = v[GAMMA],
        .rays_gain = v[RAYS_BRIGHTNESS],
        .other_width = v[OTHER_WIDTH],
        .rays_rotate = v[RAYS_ROTATE],
        .center_bright = (v[CENTER_SCALE_BRIGHTNESS] - 1.0) * wc,
        .center_width = (v[CENTER_SCALE_WIDTHS] - 1.0) * wc,
        .edge_bright = (v[EDGE_SCALE_BRIGHTNESS] - 1.0) * we,
        .edge_width = (v[EDGE_SCALE_WIDTHS] - 1.0) * we,
    };
    for (int c = 0; c < 3; ++c) {
        g->hot_gain[c] = v[HOTSPOT_BRIGHT] * v[HOTSPOT_COLOR + c];
        g->other_gain[c] = v[OTHER_BRIGHTNESS] * v[OTHER_COLOR + c];
    }
}

// The settings of the occlusion, the Flicker and the finish, from the frame's values.
static void frame_settings(const MsRender *r, Job *j) {
    const double *v = r->values;
    j->from = r->connected[OCCLUSION] ? (int)v[OCCLUSION_FROM] : 0;
    j->invert = (int)v[INVERT_OCCLUSION];
    j->hue = (float)(v[HUE_SHIFT] - floor(v[HUE_SHIFT]));
    j->saturation = (float)v[SATURATION];
    j->bg = (float)v[BG_BRIGHTNESS];
    j->combine = (int)v[COMBINE];
    j->tint = (int)v[TINT_BG_WHITES];
    j->affect_alpha = (float)v[AFFECT_ALPHA];
    split(v[BLUR_FLARE], &j->blur, &j->blur_frac);
    // The occlusion: 1 - f^2, f the Occlusion input's mean over the square of half side
    // floor(0.7 Occlusion Softness) pixels (1 at its default, 0.0028 x the frame's width at
    // 512) around the hotspot's pixel (exact in ninths at the default softness on half
    // planes, horizontal or vertical, inverted or not, and on a 2 px disc; within 1 % on
    // half planes at softness 0, 5 and 20). Occlusion Scale Brightness, Widths: per
    // element, as the triggers (ms_lensflare_draw).
    j->spot[0] = (int)floor(fmin(fmax(v[HOTSPOT], -1e6), 1e6));
    j->spot[1] = (int)floor(fmin(fmax(v[HOTSPOT_Y], -1e6), 1e6));
    j->square = (int)fmin(floor(0.7 * fmax(v[OCCLUSION_SOFTNESS], 0.0) + 1e-3), 1024.0);
    // Flicker (fitted on the reference's statistics: sd 0.18 x Amp at Randomness 0, frame to
    // frame correlation 0.66 at Speed 1, at Randomness r a dead zone of 0.175 r (44 % of the
    // frames unchanged at 0.6) and a scale (1 - r)^0.87; MillSaph's own noise, its own
    // pattern)
    j->flicker_amp = (float)v[FLICKER_AMP];
    j->flicker_z = (float)fmod(0.37 * v[FLICKER_SPEED] * r->time + 11.3, 4096.0);
    j->flicker_dead = (float)(0.175 * v[FLICKER_RANDOMNESS]);
    j->flicker_scale = (float)pow(fmax(1.0 - v[FLICKER_RANDOMNESS], 0.0), 0.87);
    j->occluded_bright = (float)(v[OCCLUSION_SCALE_BRIGHTNESS] - 1.0);
    j->occluded_width = (float)(v[OCCLUSION_SCALE_WIDTHS] - 1.0);
    for (int c = 0; c < 3; ++c)
        j->gain[c] = (float)(v[BRIGHTNESS] * v[COLOR + c]);
}

// The Atmosphere (effects/atmosphere.h).
static void atmosphere_settings(const MsRender *r, Job *j) {
    const double *v = r->values;
    const MsAtmosphere a =
        ms_atmosphere(v[ATMOSPHERE_AMP], v[ATMOSPHERE_FREQ], v[ATMOSPHERE_DETAIL],
                      v[ATMOSPHERE_SEED], v[ATMOSPHERE_SPEED], r->frame_width, r->time);
    j->amp = a.amp, j->freq = a.freq, j->persistence = a.persistence, j->z = a.z;
    j->octaves = a.octaves;
}

// The Motion Blur: the elements at `samples` times spread evenly over the shutter (Shutter
// Angle / 360 of a frame, centred on the frame, as the reference's: no shift mid-motion, half
// the blur on a key's still side), each 1 / samples as bright; samples about every
// MOTION_SPACING pixels of the hotspot's and pivot's travel, at most MOTION_SAMPLES. (The
// reference's blur of a soft blob: a box over the shutter, within 0.5 % of its energy.) The
// occlusion, the Diffraction Glow, the Atmosphere and the Flicker: at the frame. Returns the
// samples (1: no blur), the shutter in `shutter` (frames).
static int motion_samples(const MsRender *r, double *shutter) {
    const double *v = r->values;
    int samples = 1;
    *shutter = fmin(fmax(v[SHUTTER_ANGLE], 0.0), 360.0) / 360.0;
    if (v[ENABLE_MOTION_BLUR] != 0.0 && *shutter > 0 && r->values_at) {
        double a[MS_MAX_PARAMS], b[MS_MAX_PARAMS];
        if (r->values_at(r, r->time - 0.5 * *shutter, a) &&
            r->values_at(r, r->time + 0.5 * *shutter, b)) {
            const double travel = hypot(b[HOTSPOT] - a[HOTSPOT], b[HOTSPOT_Y] - a[HOTSPOT_Y]) +
                                  hypot(b[PIVOT] - a[PIVOT], b[PIVOT_Y] - a[PIVOT_Y]);
            samples = (int)fmin(ceil(travel / MOTION_SPACING), MOTION_SAMPLES);
            samples = samples > 1 ? samples : 1;
        }
    }
    return samples;
}

// The lens's rows at each of the motion blur's times (glare_style.c), one after the other
// (their shadows' references and gains made so), into `rows`, the lens's JSON tree in `nodes`
// and its images and textures in `textures` (the frame's own sample's); returns how many, or
// -1 if `lens` is not a lens.
static int build_rows(const MsRender *r, const char *lens, MsJson *nodes, int max_nodes,
                      float (*rows)[GLARE_FIELDS], MsLensTextures *textures) {
    double shutter;
    const int samples = motion_samples(r, &shutter);
    int count = 0;
    // (the frame's own sample last: `nodes` then holds the lens as `textures` describes it)
    for (int k = samples - 1; k >= 0; --k) {
        double at[MS_MAX_PARAMS];
        const double *vk = r->values;
        if (samples > 1 && r->values_at(r, r->time + shutter * ((k + 0.5) / samples - 0.5), at))
            vk = at;
        MsGlareGlobals g;
        lens_globals(vk, r->frame_width, r->frame_height, &g);
        MsLensTextures again = {.count = 0}; // (the same as the frame's)
        const int n = ms_glare_style(lens, nodes, max_nodes, &g, rows + count, GLARE_MAX_ELEMENTS,
                                     k == 0 ? textures : &again);
        if (n < 0)
            return -1;
        for (int e = count; e < count + n; ++e) {
            if (rows[e][GLARE_SHADOW_ROW] > 0)
                rows[e][GLARE_SHADOW_ROW] += (float)count;
            for (int c = 0; c < 3; ++c)
                rows[e][GLARE_GAIN_R + c] /= (float)samples;
        }
        count += n;
    }
    return count;
}

// The noisy rows' noises (radius noise, noise_brightness, the rays' streaks: the lens's
// alone), computed once for each different one (the motion blur's times share them): the
// noise rows, each noisy row's GLARE_NOISE_SLOT.
static int same_noises(const float *a, const float *b) {
    const size_t polar = (GLARE_NB_BLUR + 1 - GLARE_RN_OCTAVES) * sizeof(float);
    const size_t streaks = (GLARE_STREAK_SEED + 1 - GLARE_STREAK_AMP) * sizeof(float);
    return !memcmp(a + GLARE_RN_OCTAVES, b + GLARE_RN_OCTAVES, polar) &&
           !memcmp(a + GLARE_STREAK_AMP, b + GLARE_STREAK_AMP, streaks);
}

static void share_noises(float (*rows)[GLARE_FIELDS], int count,
                         float (*noise_rows)[GLARE_FIELDS]) {
    int noises = 0;
    for (int e = 0; e < count; ++e) {
        float *row = rows[e];
        if (!(row[GLARE_RN_OCTAVES] > 0 || row[GLARE_NB] != 0 || row[GLARE_STREAK_AMP] != 0))
            continue;
        int k = 0;
        while (k < noises && !same_noises(noise_rows[k], row))
            ++k;
        if (k == noises && noises < GLARE_MAX_ELEMENTS)
            memcpy(noise_rows[noises++], row, sizeof *noise_rows);
        row[GLARE_NOISE_SLOT] = (float)(k < noises ? k : 0);
    }
}

// The lens's images and MillSaph's textures it asks for: their texels, GLARE_TEXEL_ROW floats
// a row (one row of 0 without; `*rows`: how many), the textures generated here, on the CPU.
// Returns them (malloc), or NULL with the reason in r->err.
static float *lens_texels(const MsRender *r, const MsJson *nodes, MsLensTextures *textures,
                          int *rows) {
    const size_t count = textures->texels;
    *rows = (int)((count + GLARE_TEXEL_ROW - 1) / GLARE_TEXEL_ROW);
    float *texels = calloc((size_t)(*rows > 0 ? *rows : 1) * GLARE_TEXEL_ROW, sizeof(float));
    if (!texels) {
        snprintf(r->err, r->err_size, "not enough memory");
        return NULL;
    }
    if (count > 0)
        ms_lens_texels(nodes, textures, texels);
    for (int t = 0; t < textures->count; ++t) {
        const int status = textures->list[t].kind >= 0 // (the others: images written in the lens)
                               ? texture(&textures->list[t], texels + textures->list[t].start)
                               : 0;
        if (status != 0) {
            free(texels);
            snprintf(r->err, r->err_size, "a texture of the Lens failed (Halide error %d)", status);
            return NULL;
        }
    }
    return texels;
}

// The rows drawn (none of size 0, no shadow's own row) in lists by how (GLARE_DRAW_*: `sizes`
// their sizes), one after the other, then the shadows' rows (their references renumbered).
// Returns 0 without the memory.
static int sort_rows(float (*rows)[GLARE_FIELDS], int count, int sizes[GLARE_DRAW_LISTS]) {
    float(*sorted)[GLARE_FIELDS] = calloc(GLARE_MAX_ROWS, sizeof *sorted);
    int *at = malloc(GLARE_MAX_ROWS * sizeof(int)), n = 0;
    if (!sorted || !at) {
        free(sorted);
        free(at);
        return 0;
    }
    for (int list = 0; list <= GLARE_DRAW_LISTS; ++list)
        for (int e = 0; e < count; ++e) {
            const int drawn = rows[e][GLARE_R] > 0 && rows[e][GLARE_HIDDEN] == 0;
            const int special = rows[e][GLARE_RN_OCTAVES] > 0 || rows[e][GLARE_NB] != 0 ||
                                rows[e][GLARE_TEX] != 0 || rows[e][GLARE_TN_AMP] != 0 ||
                                rows[e][GLARE_STREAK_AMP] != 0 || rows[e][GLARE_IRREGULAR] != 0;
            const int kind = !drawn ? GLARE_DRAW_LISTS
                                    : (special ? GLARE_DRAW_SPECIAL : GLARE_DRAW_PLAIN) +
                                          (rows[e][GLARE_SHADOW_ROW] > 0);
            if (kind != list)
                continue;
            memcpy(sorted[n], rows[e], sizeof *sorted);
            at[e] = n++;
            if (list < GLARE_DRAW_LISTS)
                ++sizes[list];
        }
    for (int e = 0; e < n; ++e)
        if (sorted[e][GLARE_SHADOW_ROW] > 0)
            sorted[e][GLARE_SHADOW_ROW] = (float)(at[(int)sorted[e][GLARE_SHADOW_ROW] - 1] + 1);
    memcpy(rows, sorted, sizeof *sorted * GLARE_MAX_ROWS);
    free(sorted);
    free(at);
    return 1;
}

// The Diffraction Glow (fitted on 15 occluders and settings at 512 x 512, ~5 % L2): the
// Occlusion input blurred (sigma 0.218 x Occlusion Softness), its gradient's length x
// exp(-(d / (1.937 Glow Radius))^1.7), d the distance to the hotspot (as drawn, one pixel
// higher), blurred (sigma 0.365 x Glow Width), x 45.2 Diffraction Glow x Glow Color x the
// Hotspot Bright and Color x how much of the flare shows. Glow Radius 0: the whole edges.
// Returns 1 when there is one.
static int glow_settings(const double *v, Job *j) {
    split(box_total(0.218 * v[OCCLUSION_SOFTNESS]), &j->edge, &j->edge_frac);
    split(box_total(0.365 * v[GLOW_WIDTH]), &j->glow_blur, &j->glow_frac);
    j->glow_at[0] = (float)v[HOTSPOT];
    j->glow_at[1] = (float)(v[HOTSPOT_Y] + 1.0);
    j->reach = (float)(1.937 * v[GLOW_RADIUS]);
    for (int c = 0; c < 3; ++c)
        j->glow_gain[c] = (float)(45.23 * v[DIFFRACTION_GLOW] * v[GLOW_COLOR + c] *
                                  v[HOTSPOT_BRIGHT] * v[HOTSPOT_COLOR + c]);
    return j->from && v[DIFFRACTION_GLOW] > 0;
}

// The kernels on the rows (GLARE_MAX_ROWS), noise rows (GLARE_MAX_ELEMENTS) and texels
// (`texel_rows` rows) as `j` sets them; with the Diffraction Glow when `glow`.
static int draw(const MsRender *r, Job *j, float (*rows)[GLARE_FIELDS],
                float (*noise_rows)[GLARE_FIELDS], float *texels, int texel_rows, int glow) {
    const int w = r->width, h = r->height;
    float *flare = malloc((size_t)w * h * 4 * sizeof(float));
    float *glowed = glow ? malloc((size_t)w * h * 4 * sizeof(float)) : NULL;
    float visible[4] = {1, 1, 1, 1};
    int ok = flare != NULL && (!glow || glowed != NULL);
    if (ok) {
        MsImage source, occluder, rows_image, noise_image, texels_image, visible_image,
            flare_image, glowed_image, output;
        ms_image_rgba(&source, r->inputs[SOURCE].pixels, r->inputs[SOURCE].row, r->half, w, h);
        ms_image_rgba(&occluder, r->inputs[OCCLUSION].pixels, r->inputs[OCCLUSION].row, r->half,
                      w, h);
        ms_image(&rows_image, &rows[0][0], GLARE_FIELDS, GLARE_FIELDS, GLARE_MAX_ROWS, 1);
        ms_image(&noise_image, &noise_rows[0][0], GLARE_FIELDS, GLARE_FIELDS, GLARE_MAX_ELEMENTS,
                 1);
        ms_image(&texels_image, texels, GLARE_TEXEL_ROW, GLARE_TEXEL_ROW,
                 texel_rows > 0 ? texel_rows : 1, 1);
        ms_image(&visible_image, visible, 4, 1, 1, 4);
        ms_image(&flare_image, flare, (ptrdiff_t)w * 4, w, h, 4);
        if (glow)
            ms_image(&glowed_image, glowed, (ptrdiff_t)w * 4, w, h, 4);
        ms_image_rgba(&output, r->output, r->output_row, r->half, w, h);
        // Without an Occlusion input, or one that is the Source's image in the host: the
        // Source stands in (not read without one).
        const int same = j->from && ms_same_input(&r->inputs[OCCLUSION], &r->inputs[SOURCE]);
        j->source = &source;
        j->occluder = j->from && !same ? &occluder : &source;
        j->rows = &rows_image;
        j->noise_rows = &noise_image;
        j->texels = &texels_image;
        j->visible = &visible_image;
        j->flare = &flare_image;
        j->glowed = glow ? &glowed_image : NULL;
        j->output = &output;
        // Inputs uploaded to the GPU: the Source, the style's rows and noise rows, the lens's
        // texels, the Occlusion input.
        MsImage *inputs[] = {&source, &rows_image, &noise_image, &texels_image, j->occluder};
        MsImage *between[] = {&visible_image, &flare_image, &glowed_image};
        ok = ms_run(run, j, inputs, j->occluder == &source ? 4 : 5, between, glow ? 3 : 2,
                    &output, r->info, r->err, r->err_size);
    } else {
        snprintf(r->err, r->err_size, "not enough memory");
    }
    free(glowed);
    free(flare);
    return ok;
}

static int render(const MsRender *r) {
    const double *v = r->values;
    // the lens: a preset, else the Lens text (hosts without it: the default lens)
    const int preset = (int)v[PRESET];
    const char *lens = preset > 0 && preset <= MS_LENSFLARE_PRESETS
                           ? ms_lensflare_preset_texts[preset - 1]
                       : r->texts && r->texts[LENS] ? r->texts[LENS]
                                                    : params[LENS].def_text;
    Job j = {.count = 0};
    frame_settings(r, &j);
    atmosphere_settings(r, &j);
    const int glow = glow_settings(v, &j);

    const size_t text_length = strlen(lens);
    MsJson *nodes = malloc((text_length + 1) * sizeof(MsJson));
    float(*rows)[GLARE_FIELDS] = calloc(GLARE_MAX_ROWS, sizeof *rows);
    float(*noise_rows)[GLARE_FIELDS] = calloc(GLARE_MAX_ELEMENTS, sizeof *noise_rows);
    float *texels = NULL;
    MsLensTextures textures = {.count = 0};
    int texel_rows = 0, ok = 0;
    if (!nodes || !rows || !noise_rows) {
        snprintf(r->err, r->err_size, "not enough memory");
    } else if ((j.count = build_rows(r, lens, nodes, (int)text_length + 1, rows, &textures)) < 0) {
        snprintf(r->err, r->err_size, "the Lens is not a lens (JSON)");
    } else {
        share_noises(rows, j.count, noise_rows);
        texels = lens_texels(r, nodes, &textures, &texel_rows);
        if (texels && !sort_rows(rows, j.count, j.sizes))
            snprintf(r->err, r->err_size, "not enough memory");
        else if (texels)
            ok = draw(r, &j, rows, noise_rows, texels, texel_rows, glow);
    }
    free(nodes);
    free(texels);
    free(noise_rows);
    free(rows);
    return ok;
}

// No flare (Brightness 0, Color 0 or Scale Widths 0) over the whole Source, screened or
// added: the Source as it is.
static int identity(const double *v, const int *connected) {
    (void)connected;
    const int no_flare = v[SCALE_WIDTHS] <= 0.0 || v[BRIGHTNESS] == 0.0 ||
                         (v[COLOR] == 0.0 && v[COLOR_G] == 0.0 && v[COLOR_B] == 0.0);
    return no_flare && v[BG_BRIGHTNESS] == 1.0 && (int)v[COMBINE] != 2;
}

const MsEffect ms_lensflare_effect = {
    MS_ID("LensFlare"),
    "MS_LensFlare",
    "A lens flare: glows, rays, rings and ghosts along the line from the pivot through the "
    "hotspot.",
    clips,
    ARRAY_SIZE(clips),
    params,
    ARRAY_SIZE(params),
    render,
    identity,
    1, // the Atmosphere changes over time
    release,
    {"hotspot", "pivot"}, // the axis in the viewer
    "lensflare", // presets (common/preset.h)
    NULL, // not varying with its settings
};
