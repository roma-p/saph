// MS_LensFlare's elements: what only they have, on top of glare_style.c's rows (see
// glare_build.h): their place on the axis from the pivot through the hotspot, their noises,
// shift_edge, shadows and images (the lens's own, written in the element, or MillSaph's
// generated textures).

#include <string.h>

#include "effects/glare_build.h"

// The noise_amp texture's cells a radius per simplex_noise_freq, its change with the
// element's position (per S), the streaks' cells a radian per streaks_freq (fitted on the
// reference's correlation lengths)
#define TEXTURE_SCALE 1.95
#define TEXTURE_VARY 2.8
#define STREAK_SCALE 2.15

// A distance law's factor (scale s at distance d; 1 without one).
static double by_dist(const GlareBuilder *b, int e, double s, double d, const char *inverted,
                      const char *min_scale) {
    if (s == 0.0)
        return 1.0;
    // (the distance less offset_distance x the frame's width, at least 0: 4 x it in units of
    // a quarter width; measured at 4 offsets x 4 distances)
    d = fmax(d - 4.0 * get(b, e, "offset_distance", 0.0), 0.0);
    // min_*_scale m: added to s d, a floor under 1 - s d (inverted); measured at m 0.3 both
    // ways, 0.5 and 0.605 inverted
    const double m = get(b, e, min_scale, 0.0);
    if (get(b, e, inverted, 0.0) != 0.0)
        return fmax(fmax(1.0 - s * d, m), 0.0);
    return fmax(m + s * d, 0.0);
}

// Element e's place on the axis from the pivot through the hotspot (its row's position and
// rotation), its size factor, what its stretch and the stretch's inverse gain, and its
// brightness factors there.
void ms_lens_place(const GlareBuilder *b, int e, float *row, double *size_k, double stretch_add[2],
                   double gain[3]) {
    const MsGlareGlobals *g = b->g;
    const double ax = g->hotspot[0] - g->pivot[0], ay = g->hotspot[1] - g->pivot[1];
    const double axis_length = sqrt(ax * ax + ay * ay);
    const double ux = axis_length > 0 ? ax / axis_length : 1.0;
    const double uy = axis_length > 0 ? ay / axis_length : 0.0;
    // d: the hotspot's distance to the pivot (for every element), in units of a quarter of
    // the frame's width
    const double d = axis_length / g->unit;
    // rel_speed s: its center c moved to c x s^(d / 2) (s 0.5, 1.5, 1.81, 2.5: exact)
    const double c = get(b, e, "center", 1.0) *
                     pow(fmax(get(b, e, "rel_speed", 1.0), 1e-6), d / 2.0);
    // offset_off_axis: across the axis (turned a quarter turn counter-clockwise), x the frame
    double px = g->pivot[0] + c * ax, py = g->pivot[1] + c * ay;
    const double off = get(b, e, "offset_off_axis", 0.0) * g->frame_width;
    px -= uy * off;
    py += ux * off;
    // offset_mode "horizontal follow": at the hotspot's height. unscaled_offset (x, y): moved
    // by x, y x the frame's width; scaled_offset: by 2 x (hotspot - pivot) x it, y the
    // other way (no y when following); offset_x / y themselves and offset_distance: no
    // move on a still (measured with the hotspot above left of the pivot; offset_distance
    // shortens the distance the *_by_dist scales read: by_dist)
    const int follow = ms_json_is(b->nodes, e, "offset_mode", "horizontal follow");
    double unscaled[2] = {0, 0}, scaled[2] = {0, 0};
    get_list(b, e, "unscaled_offset", unscaled, 2);
    get_list(b, e, "scaled_offset", scaled, 2);
    if (follow)
        py = g->hotspot[1];
    px += unscaled[0] * g->frame_width + 2.0 * scaled[0] * ax;
    py += unscaled[1] * g->frame_width - (follow ? 0.0 : 2.0 * scaled[1] * ay);
    row[GLARE_POS_X] = (float)px;
    row[GLARE_POS_Y] = (float)py;
    const double sbd = get(b, e, "scale_brightness_by_dist", 0.0);
    const double swd = get(b, e, "scale_width_by_dist", 0.0);
    // offset_stretch_by_dist o: a blob scaled by 1 / sqrt(1 + o d); the others' stretch s
    // made s + o d (wider in their own frame, their area kept), 1 / (1 / s + o d) with
    // stretch_offset_inverted (taller; stretch_direction: no change). 12 polygons at 2
    // distances, a fancy spot's ellipse at s 0.038 to 2, o 0 to 5.34, both ways: exact.
    const double o = get(b, e, "offset_stretch_by_dist", 0.0);
    const int round_blob = is_type(b, e, "blob");
    const double shrink = round_blob ? 1.0 / sqrt(fmax(1.0 + o * d, 1e-6)) : 1.0;
    const int inverted = get(b, e, "stretch_offset_inverted", 0.0) != 0.0;
    stretch_add[0] = round_blob || inverted ? 0.0 : o * d;
    stretch_add[1] = round_blob || !inverted ? 0.0 : o * d;
    // rotate_to_pivot: turned along the axis (from the pivot to the hotspot), minus a half
    // turn, wherever the element is (center below 0 or 0, off the axis, offset: the same
    // turn, measured on a polar flare's sector)
    if (get(b, e, "rotate_to_pivot", 0.0) != 0.0)
        row[GLARE_ROTATE] += (float)(atan2(uy, ux) - PI);
    // the families: at the hotspot (center 1), the others; the rays also by their own knobs
    const int at_hotspot = fabs(get(b, e, "center", 1.0) - 1.0) < 1e-6;
    const int rays = is_type(b, e, "rays");
    // the triggers, as much as the element responds to them (center_trigger, edge_trigger 0:
    // not to that one)
    const double rb = get(b, e, "brightness_response", 1.0);
    const double rw = get(b, e, "width_response", 1.0);
    const double ct = get(b, e, "center_trigger", 1.0) != 0.0;
    const double et = get(b, e, "edge_trigger", 1.0) != 0.0;
    row[GLARE_BRIGHT_RESPONSE] = (float)rb;
    row[GLARE_WIDTH_RESPONSE] = (float)rw;
    const double triggers_bright =
        (1.0 + ct * g->center_bright * rb) * (1.0 + et * g->edge_bright * rb);
    const double triggers_width =
        (1.0 + ct * g->center_width * rw) * (1.0 + et * g->edge_width * rw);
    // scale_brightness_by_dist s: x (min_brightness_scale + s d), or x max(1 - s d, min) when
    // brightness_scale_inverted; scale_width_by_dist the same with its own settings
    // (brightness_direction, size_direction: no change on a still)
    const double by_bright = by_dist(b, e, sbd, d, "brightness_scale_inverted",
                                     "min_brightness_scale");
    const double by_width = by_dist(b, e, swd, d, "width_scale_inverted", "min_width_scale");
    *size_k = by_width * shrink * (at_hotspot ? 1.0 : g->other_width) * triggers_width;
    row[GLARE_TURNED_FRAME] = 1.0f;
    // stretch_distort d: d |log2 stretch|^1.46 (none without a stretch; ms_glare::element_value)
    row[GLARE_DISTORT] = (float)(get(b, e, "stretch_distort", 0.0) *
                                 pow(fabs(log2(fmax(get(b, e, "stretch", 1.0), 1e-6))), 1.46));
    row[GLARE_ATMOSPHERE] = (float)(get(b, e, "ignore_atmosphere", 0.0) == 0.0);
    for (int k = 0; k < 3; ++k)
        gain[k] = by_bright * (at_hotspot ? g->hot_gain[k] : g->other_gain[k]) *
                  (rays ? g->rays_gain : 1.0) * triggers_bright;
    if (rays)
        row[GLARE_ROTATE] += (float)(g->rays_rotate * RADIANS);
}

// noise_amp: the element x max(1 + 0.714 amp n, 0), n MillSaph's fbm (of gradient noise, sd
// 0.28 an octave: the reference's sd 0.2 x amp at octaves_scale 0.2) across the element,
// simplex_noise_freq x TEXTURE_SCALE cells a radius (x freq_rel_x, freq_rel_y on the screen),
// octaves_scale x the amplitude an octave; changing as the element moves x
// noise_vary_by_position (the reference's: half decorrelated at about 25 px of travel at 1 for
// a radius of 44 px).
void ms_lens_texture_noise(const GlareBuilder *b, int e, float *row, double radius) {
    const double S = b->g->size;
    const double amp = get(b, e, "noise_amp", 0.0);
    if (amp == 0.0 || !(radius > 0))
        return;
    const double k = get(b, e, "simplex_noise_freq", 4.0) * TEXTURE_SCALE / radius;
    const double vary = get(b, e, "noise_vary_by_position", 0.0) * TEXTURE_VARY / S;
    row[GLARE_TN_AMP] = (float)(0.714 * amp);
    row[GLARE_TN_FX] = (float)(k * get(b, e, "freq_rel_x", 1.0));
    row[GLARE_TN_FY] = (float)(k * get(b, e, "freq_rel_y", 1.0));
    const double px = row[GLARE_POS_X], py = row[GLARE_POS_Y];
    row[GLARE_TN_Z] = (float)fmod(0.3 + 37.1 * get(b, e, "rand_seed", 0.0) +
                                      vary * (0.6 * px + 0.8 * py),
                                  4096.0);
    row[GLARE_TN_U] = (float)fmod(vary * (0.6 * py - 0.8 * px), 4096.0);
    row[GLARE_TN_PERSIST] = (float)get(b, e, "octaves_scale", 0.2);
}

// Streaks: stripes along the lines, each line x max(1 + 0.66 amp n, 0), n MillSaph's fbm (5
// octaves, persistence streaks_octaves_scale) of t x streaks_freq^0.87 x STREAK_SCALE, t the
// offset across the line / its width: the same on every line, along all of it (the
// reference's: a pattern of the offset across / the width alone, the energy's factor the
// same at any width and number of lines; mean -1 % (within its spread), sd 0.19 x amp
// across at freq 1 and persistence 0.4, 1.2, 2.3, 3.9, 6.8 crossings a width at freq 0.5,
// 1, 2, 4).
void ms_lens_streaks(const GlareBuilder *b, int e, float *row) {
    const double streaks = get(b, e, "streaks_amp", 0.0);
    if (streaks == 0.0)
        return;
    row[GLARE_STREAK_AMP] = (float)streaks;
    row[GLARE_STREAK_FREQ] = (float)(pow(fmax(get(b, e, "streaks_freq", 1.0), 0.0), 0.87) * STREAK_SCALE);
    row[GLARE_STREAK_PERSIST] = (float)get(b, e, "streaks_octaves_scale", 0.3);
    row[GLARE_STREAK_SEED] = (float)(0.7 + 37.1 * get(b, e, "rand_seed", 0.0));
}

// shift_edge: the element moved along its own x by that x its outer radius (`stop`, the last
// stop's radius, x `R`; a polar flare: x 0.937), squeezed by rel_width, rel_height on the
// screen, its size kept (measured on polar flares and a fancy spot, with and without
// rotate_to_pivot).
void ms_lens_shift_edge(const GlareBuilder *b, int e, float *row, double stop, double R,
                        int polar) {
    const double shift = get(b, e, "shift_edge", 0.0) * stop * R * (polar ? 0.937 : 1.0);
    row[GLARE_POS_X] += (float)(shift * cos(row[GLARE_ROTATE]) * row[GLARE_REL_X]);
    row[GLARE_POS_Y] += (float)(shift * sin(row[GLARE_ROTATE]) * row[GLARE_REL_Y]);
}

// A polar flare's radius noise and noise_brightness.
void ms_lens_polar_noises(const GlareBuilder *b, int e, float *row) {
    if (get(b, e, "r_noise_min", 1.0) != 1.0 || get(b, e, "r_noise_max", 1.0) != 1.0) {
        // The radius noise: x r_noise_min..max by MillSaph's noise around the element,
        // about r_noise_freq^0.785 x 1.38 cycles a turn (as the reference's: 4.9 at 5, 14.5 at
        // 20), sd 0.142 x the range at one octave, octaves of persistence 0.43 averaged
        const int octaves = (int)fmin(fmax(get(b, e, "r_noise_octaves", 1.0), 1.0), 8.0);
        double total = 0.0;
        for (int k = 0; k < octaves; ++k)
            total += pow(0.43, k);
        row[GLARE_RN_OCTAVES] = (float)octaves;
        row[GLARE_RN_MIN] = (float)get(b, e, "r_noise_min", 1.0);
        row[GLARE_RN_MAX] = (float)get(b, e, "r_noise_max", 1.0);
        row[GLARE_RN_RHO] = (float)(0.529 * pow(fmax(get(b, e, "r_noise_freq", 5.0), 0.0), 0.785));
        row[GLARE_RN_SEED] = (float)(0.3 + 37.1 * get(b, e, "rand_seed", 0.0));
        row[GLARE_RN_SCALE] = (float)(0.59 / total);
    }
    if (get(b, e, "noise_brightness", 0.0) != 0.0) {
        // noise_brightness: x it x MillSaph's noise around the element (as the radius
        // noise's: 0.5 + 0.4 x perlin, sd 0.1), thresholded by noise_width and
        // noise_softness as the waves, averaged over blur_theta_map x a turn. The reference's
        // noise is that narrow with noise_boil_speed (0.3 and up), twice as wide and about 0.04
        // lower without it (hard thresholds at 5 widths x 6 frequencies, 5 to 1200; 6 seeds at
        // 3, 20 and 120), between the two as exp(-speed / 0.157) (0 to 0.5): the scale 0.4 +
        // 0.4 k, the mean 0.5 - 0.04 k at one octave (as a narrower width: the same threshold),
        // k that factor; with more octaves, as much narrower as ours, its mean 0.5.
        const int octaves = (int)fmin(fmax(get(b, e, "noise_octaves", 1.0), 1.0), 8.0);
        double total = 0.0;
        for (int k = 0; k < octaves; ++k)
            total += pow(0.43, k);
        const double still = exp(-fabs(get(b, e, "noise_boil_speed", 0.0)) / 0.157);
        const double width = get(b, e, "noise_width", 0.3);
        const double soft = fmax(get(b, e, "noise_softness", 1.0), 0.0);
        row[GLARE_NB] = (float)get(b, e, "noise_brightness", 0.0);
        const double lower = octaves > 1 ? 0.0 : 0.04 * still;
        row[GLARE_NB_WIDTH] = (float)(width >= 1.0 ? width : width - lower / (soft + 1.0));
        row[GLARE_NB_SOFT] = (float)get(b, e, "noise_softness", 1.0);
        row[GLARE_NB_RHO] = (float)(0.529 * pow(fmax(get(b, e, "noise_freq", 120.0), 0.0), 0.785));
        row[GLARE_NB_SEED] = (float)(0.7 + 37.1 * get(b, e, "rand_seed", 0.0));
        row[GLARE_NB_OCTAVES] = (float)octaves;
        row[GLARE_NB_SCALE] = (float)((0.4 + 0.4 * still) / total);
        row[GLARE_NB_BLUR] = (float)fmin(fmax(get(b, e, "blur_theta_map", 0.0), 0.0), 1.0);
    }
}

// The shadow's move (in units of its element's radius, per unit of shadow_amount_at_edge)
// along one of the screen's axes, at q: how far its reference point (the hotspot, or the
// element with shadow_dist_calc "use element") is from the pivot along that axis, in frame
// widths, and d: shadow_animation_delay. None up to q = d, then (q - d) / (0.96 - q); at
// delay 0, 1.02 q / (1 - q)^0.15. Away from the reference point's side. (Fitted on 9
// distances from 1/16 to 3/4 of the frame's width x 6 delays from 0 to 0.5, either axis
// and both, within 0.01 R; linear in the amount from -20 to 11 and in the radius.)
static double shadow_law(double q, double d) {
    if (d <= 0.0)
        return 1.02 * q / pow(fmax(1.0 - q, 0.01), 0.15);
    return q <= d ? 0.0 : (q - d) / fmax(0.96 - q, 0.01);
}

// Element e (just added, row `owner`) masked by the lens's shadow when it asks
// (use_shadow_p): a row of the shadow element of radius (R + 1.1 px) x shadow_rel_scale (R:
// the element's; the reference's shadow edge about a pixel further out than its elements'),
// centred on the element moved by R x amount x shadow_law along each axis
// (shadow_amount_at_edge), then by R x shadow_offset_along_axis away from the pivot along
// the axis from the hotspot and R x shadow_offset_off_axis across it; those moves stretched
// as the element (rel_width, rel_height on the screen, 1 / sqrt(its stretch) both ways).
void ms_lens_shadow(GlareBuilder *b, int e, int owner) {
    const MsGlareGlobals *g = b->g;
    if (b->shadow < 0 || get(b, e, "use_shadow_p", 0.0) == 0.0 || b->count >= b->max ||
        !(g->size > 0) || !(g->frame_width > 0))
        return;
    const double R = b->last_r;
    const double ax = g->pivot[0] - g->hotspot[0], ay = g->pivot[1] - g->hotspot[1];
    const double length = sqrt(ax * ax + ay * ay);
    const double ux = length > 0 ? ax / length : 1.0, uy = length > 0 ? ay / length : 0.0;
    float *row = b->rows[owner];
    // the reference point, from the pivot
    const int use_element = ms_json_is(b->nodes, e, "shadow_dist_calc", "use element");
    const double px = use_element ? row[GLARE_POS_X] - g->pivot[0] : -ax;
    const double py = use_element ? row[GLARE_POS_Y] - g->pivot[1] : -ay;
    const double delay = fmax(get(b, e, "shadow_animation_delay", 0.1), 0.0);
    const double amount = get(b, e, "shadow_amount_at_edge", 1.0);
    const double W = g->frame_width;
    const double law_x = px == 0.0 ? 0.0 : -copysign(shadow_law(fabs(px) / W, delay), px);
    const double law_y = py == 0.0 ? 0.0 : -copysign(shadow_law(fabs(py) / W, delay), py);
    const double rel_scale = get(b, e, "shadow_rel_scale", 1.0);
    const double along = -get(b, e, "shadow_offset_along_axis", 0.0);
    const double across = get(b, e, "shadow_offset_off_axis", 0.0);
    b->as_shadow = 1;
    b->shadow_r = R * rel_scale + 1.1 * rel_scale;
    const double rx = row[GLARE_REL_X] > 0 ? row[GLARE_REL_X] : 1.0;
    const double ry = row[GLARE_REL_Y] > 0 ? row[GLARE_REL_Y] : 1.0;
    // (measured at stretches 0.3 and 4.95, a needle at 0.038 out of its shadow)
    const double k = row[GLARE_SCALE_X];
    const double tx = k * (amount * law_x + along * ux - across * uy);
    const double ty = k * (amount * law_y + along * uy + across * ux);
    b->shadow_x = row[GLARE_POS_X] + R * rx * tx;
    b->shadow_y = row[GLARE_POS_Y] + R * ry * ty;
    const int at = b->count;
    ms_style_add_element(b, b->shadow);
    b->as_shadow = 0;
    if (b->count == at)
        return;
    b->rows[at][GLARE_HIDDEN] = 1.0f;
    // the shadow as wide and tall as its element (its rel_width, rel_height on the screen)
    b->rows[at][GLARE_TURNED_FRAME] = 1.0f;
    b->rows[at][GLARE_REL_X] *= row[GLARE_REL_X];
    b->rows[at][GLARE_REL_Y] *= row[GLARE_REL_Y];
    // (and stretched as it, in the shadow's own turned frame: its rotation, not the
    // element's; measured on an ellipse and a needle, centred and offset)
    b->rows[at][GLARE_SCALE_X] *= row[GLARE_SCALE_X];
    b->rows[at][GLARE_SCALE_Y] *= row[GLARE_SCALE_Y];
    row[GLARE_SHADOW_ROW] = (float)(at + 1);
    row[GLARE_SHADOW_INVERT] = (float)(get(b, e, "invert_shadow", 0.0) != 0.0);
    row[GLARE_SHADOW_OPACITY] = (float)(1.0 - get(b, e, "shadow_transparency", 0.0));
}

// Image k of the lens (`images`, an array): its size and channels, and where its
// texels start (floats, RGB); 0 if there is none (or it is not one).
static int image_at(const MsJson *nodes, int images, int k, int *w, int *h, int *channels,
                    size_t *start) {
    size_t at = 0;
    int i = 0;
    if (images < 0 || nodes[images].type != MS_JSON_ARRAY)
        return 0;
    for (int m = nodes[images].first; m >= 0; m = nodes[m].next, ++i) {
        const int iw = (int)ms_json_get(nodes, m, "xdim", 0.0);
        const int ih = (int)ms_json_get(nodes, m, "ydim", 0.0);
        const int ic = (int)ms_json_get(nodes, m, "elnum", 3.0);
        const int pixels = ms_json_member(nodes, m, "pixels");
        const int ok = iw > 0 && ih > 0 && iw <= 16384 && ih <= 16384 && (ic == 1 || ic == 3) &&
                       pixels >= 0 && nodes[pixels].type == MS_JSON_STRING &&
                       (size_t)nodes[pixels].length * 3 / 4 >= (size_t)iw * ih * ic;
        if (i == k) {
            *w = iw, *h = ih, *channels = ic, *start = at;
            return ok;
        }
        if (ok)
            at += (size_t)iw * ih * 3;
    }
    return 0;
}

// The lens's "images" member.
static int images_of(const MsJson *nodes) {
    const int value = ms_json_member(nodes, 0, "value");
    return ms_json_member(nodes, value >= 0 ? value : 0, "images");
}

size_t ms_lens_texel_count(const MsJson *nodes) {
    const int images = images_of(nodes);
    size_t total = 0, start;
    int w, h, channels;
    for (int k = 0; images >= 0 && k < nodes[images].count; ++k)
        if (image_at(nodes, images, k, &w, &h, &channels, &start))
            total = start + (size_t)w * h * 3;
    return total;
}

// The value of base64 character ch (-1: not one).
static int base64(char ch) {
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    return ch == '+' ? 62 : ch == '/' ? 63 : -1;
}

// The pixels of `text` (base64, w x h x channels bytes) into `out` as RGB floats.
static void decode(const MsJson *text, int w, int h, int channels, float *out) {
    // 4 characters, 3 bytes; each byte a channel of a texel (grey: repeated)
    const size_t bytes = (size_t)w * h * channels;
    unsigned bits = 0;
    int held = 0;
    size_t n = 0;
    for (int c = 0; c < text->length && n < bytes; ++c) {
        const int v = base64(text->text[c]);
        if (v < 0)
            continue;
        bits = (bits << 6) | (unsigned)v;
        held += 6;
        if (held >= 8) {
            held -= 8;
            const float byte = (float)((bits >> held) & 255u) / 255.0f;
            if (channels == 3)
                out[n] = byte;
            else
                out[3 * n] = out[3 * n + 1] = out[3 * n + 2] = byte;
            ++n;
        }
    }
}

void ms_lens_texels(const MsJson *nodes, const MsLensTextures *textures, float *out) {
    const int images = images_of(nodes);
    size_t start;
    int w, h, channels;
    for (int k = 0; images >= 0 && k < nodes[images].count; ++k) {
        if (!image_at(nodes, images, k, &w, &h, &channels, &start))
            continue;
        int m = nodes[images].first;
        for (int i = 0; i < k; ++i)
            m = nodes[m].next;
        decode(&nodes[ms_json_member(nodes, m, "pixels")], w, h, channels, out + start);
    }
    for (int t = 0; textures && t < textures->count; ++t) {
        const MsLensTexture *in = &textures->list[t];
        if (in->kind < 0)
            decode(&nodes[ms_json_member(nodes, in->node, "pixels")], in->width, in->height,
                   in->channels, out + in->start);
    }
}

// An image written in the element itself (its "image" with "pixels", "xdim",
// "ydim", "elnum"), added after the lens's own images; its size and first texel.
static int written(GlareBuilder *b, int image, int *w, int *h, size_t *start) {
    MsLensTexture *list = b->textures ? b->textures->list : NULL;
    const int iw = (int)ms_json_get(b->nodes, image, "xdim", 0.0);
    const int ih = (int)ms_json_get(b->nodes, image, "ydim", 0.0);
    const int ic = (int)ms_json_get(b->nodes, image, "elnum", 3.0);
    const int pixels = ms_json_member(b->nodes, image, "pixels");
    if (!list || iw <= 0 || ih <= 0 || iw > 16384 || ih > 16384 || (ic != 1 && ic != 3) ||
        pixels < 0 || b->nodes[pixels].type != MS_JSON_STRING ||
        (size_t)b->nodes[pixels].length * 3 / 4 < (size_t)iw * ih * ic)
        return 0;
    for (int i = 0; i < b->textures->count; ++i)
        if (list[i].kind < 0 && list[i].node == image) {
            *w = iw, *h = ih, *start = list[i].start;
            return 1;
        }
    if (b->textures->count >= MS_LENS_MAX_GENERATED)
        return 0;
    MsLensTexture *in = &list[b->textures->count++];
    memset(in, 0, sizeof *in);
    in->kind = -1;
    in->width = iw, in->height = ih, in->channels = ic, in->node = image;
    in->start = b->textures->texels;
    b->textures->texels += (size_t)iw * ih * 3;
    *w = iw, *h = ih, *start = in->start;
    return 1;
}

// One of MillSaph's own textures, as `image` ({"generate": ...}) asks: added to
// the ones to generate (the same settings: the same texture); its size and first texel.
static int generated(GlareBuilder *b, int image, int *w, int *h, size_t *start) {
    static const char *const kinds[] = {"dirt", "glint", "ramp", "iris"};
    static const int sizes[] = {512, 256, 128, 256}; // texels a side by default
    MsLensTextures *t = b->textures;
    int kind = -1;
    for (int k = 0; k < 4; ++k)
        if (ms_json_is(b->nodes, image, "generate", kinds[k]))
            kind = k;
    if (!t || kind < 0)
        return 0;
    const int side = (int)fmin(fmax(ms_json_get(b->nodes, image, "texels", sizes[kind]), 8.0),
                               2048.0);
    const MsLensTexture want = {kind,
                                ms_json_get(b->nodes, image, "seed", 0.0),
                                ms_json_get(b->nodes, image, "amount", kind == 0 ? 0.35 : kind == 2 ? 1.0 : 0.5),
                                ms_json_get(b->nodes, image, "scale", 1.0),
                                ms_json_get(b->nodes, image, "detail", 0.5),
                                (int)ms_json_get(b->nodes, image, "sides", kind == 3 ? 6 : 0),
                                side,
                                side,
                                ms_json_get(b->nodes, image, "mean", 0.0),
                                t->texels,
                                -1,
                                3};
    for (int i = 0; i < t->count; ++i) {
        const MsLensTexture *have = &t->list[i];
        if (have->kind == want.kind && have->seed == want.seed && have->amount == want.amount &&
            have->scale == want.scale && have->detail == want.detail &&
            have->sides == want.sides && have->width == want.width &&
            have->mean == want.mean) {
            *w = have->width, *h = have->height, *start = have->start;
            return 1;
        }
    }
    if (t->count >= MS_LENS_MAX_GENERATED)
        return 0;
    t->list[t->count++] = want;
    t->texels += (size_t)side * side * 3;
    *w = side, *h = side, *start = want.start;
    return 1;
}

// Element e's image (its "image": {"save_index": k} or {"generate": ...}) on its
// row (just added):
// where it is and how big (fitted on MillSaph's test images at 512 x 512, exact maps): the
// side that fits (the shorter one; a texture: the longer) 2 (R + 1.5) px x
// image_scale_size (image_absolute_size_p: the frame's width; a texture: 2 R, no image_*
// setting), its columns turned by rotate + image_offset_rotate (+ the angle from the pivot
// with rotate_to_pivot; image_absolute_rotate_p: image_offset_rotate only), its rows a
// quarter turn clockwise from them, then stretched as the element (in the element's frame;
// image_absolute_rotate_p: in the screen's, the element's turn left out: measured on a polar
// flare turned 114 deg, stretch 0.33 to 6 and offset_stretch_by_dist, both ways),
// centred on the element (image_absolute_position_p: the pivot) moved by image_offset_x, y
// x half that side. A texture: the image x its colours (not its brightness).
void ms_lens_image(GlareBuilder *b, int e, float *row) {
    const MsGlareGlobals *g = b->g;
    const int image = ms_json_member(b->nodes, e, "image");
    int w, h, channels;
    size_t start;
    const int texture = is_type(b, e, "texture");
    int found = image >= 0 && b->images >= 0 &&
                image_at(b->nodes, b->images,
                         (int)ms_json_get(b->nodes, image, "save_index", -1.0), &w, &h,
                         &channels, &start);
    if (image >= 0 && !found)
        found = generated(b, image, &w, &h, &start) || written(b, image, &w, &h, &start);
    if (!found || start + (size_t)w * h * 3 >= (1u << 24)) { // (indices exact in floats)
        if (texture) // (a texture without its image: nothing)
            row[GLARE_R] = 0.0f;
        return;
    }
    double side = !texture && get(b, e, "image_absolute_size_p", 0.0) != 0.0 ? g->frame_width
                  : texture                                                 ? 2.0 * b->last_r
                                                       : 2.0 * (b->last_r + 1.5);
    if (!texture)
        side *= get(b, e, "image_scale_size", 1.0);
    const double texel = side / (texture ? fmax(w, h) : fmin(w, h)); // pixels
    double angle = texture ? 0.0 : get(b, e, "image_offset_rotate", 0.0) * RADIANS;
    if (texture || get(b, e, "image_absolute_rotate_p", 0.0) == 0.0) {
        angle += get(b, e, "rotate", 0.0) * RADIANS;
        const double dx = row[GLARE_POS_X] - g->pivot[0], dy = row[GLARE_POS_Y] - g->pivot[1];
        if (get(b, e, "rotate_to_pivot", 0.0) != 0.0 && (dx != 0 || dy != 0))
            angle += atan2(dy, dx);
    }
    // a pixel's offset to texels: undo the element's stretch in its frame (turned by
    // GLARE_ROTATE: x / (rel_width / sqrt(s)), y / (rel_height sqrt(s))), then turn into the
    // image's columns (cos a, sin a) and rows (sin a, -cos a), / the texel
    // U: a pixel's offset into the element's frame, unstretched and unscaled, turned back to
    // the screen's axes: R(rot) diag(1 / kx, 1 / ky) R(-rot) diag(1 / rw, 1 / rh), as
    // ms_glare::element_value
    const double sx = fmax(row[GLARE_SCALE_X], 1e-6), sy = fmax(row[GLARE_SCALE_Y], 1e-6);
    const double rw = fmax(row[GLARE_REL_X], 1e-6);
    const double rh = fmax(row[GLARE_REL_Y], 1e-6);
    const double rot = !texture && get(b, e, "image_absolute_rotate_p", 0.0) != 0.0
                           ? 0.0 : row[GLARE_ROTATE];
    double m[2][2] = {{1.0 / rw, 0.0}, {0.0, 1.0 / rh}};
    const double cb = cos(rot), sb = sin(rot);
    double t[2][2] = {{(cb * m[0][0] + sb * m[1][0]) / sx, (cb * m[0][1] + sb * m[1][1]) / sx},
                      {(-sb * m[0][0] + cb * m[1][0]) / sy, (-sb * m[0][1] + cb * m[1][1]) / sy}};
    const double cr = cos(rot), sr = sin(rot);
    const double u00 = cr * t[0][0] - sr * t[1][0], u01 = cr * t[0][1] - sr * t[1][1];
    const double u10 = sr * t[0][0] + cr * t[1][0], u11 = sr * t[0][1] + cr * t[1][1];
    const double ca = cos(angle), sa = sin(angle);
    double cx = row[GLARE_POS_X], cy = row[GLARE_POS_Y];
    if (!texture && get(b, e, "image_absolute_position_p", 0.0) != 0.0)
        cx = g->pivot[0], cy = g->pivot[1];
    if (!texture) {
        cx += get(b, e, "image_offset_x", 0.0) * side / 2.0;
        cy += get(b, e, "image_offset_y", 0.0) * side / 2.0;
    }
    row[GLARE_TEX] = 1.0f;
    row[GLARE_TEX_START] = (float)start;
    row[GLARE_TEX_W] = (float)w;
    row[GLARE_TEX_H] = (float)h;
    row[GLARE_TEX_X] = (float)cx;
    row[GLARE_TEX_Y] = (float)cy;
    row[GLARE_TEX_M00] = (float)((ca * u00 + sa * u10) / texel);
    row[GLARE_TEX_M01] = (float)((ca * u01 + sa * u11) / texel);
    row[GLARE_TEX_M10] = (float)((sa * u00 - ca * u10) / texel);
    row[GLARE_TEX_M11] = (float)((sa * u01 - ca * u11) / texel);
    if (texture) {
        // the image alone, as far as its corners reach, x its colours (not its brightness)
        const double reach = 0.5 * texel * hypot(w, h) * fmax(fmax(sx, sy), 1.0) *
                             fmax(fmax(row[GLARE_REL_X], row[GLARE_REL_Y]), 1.0);
        double bright = get(b, e, "brightness", 1.0);
        if (g->gamma > 0 && bright > 0)
            bright = pow(bright, 1.0 / g->gamma);
        row[GLARE_KIND] = GLARE_IMAGE;
        row[GLARE_R] = (float)(reach + 2.0 * texel);
        row[GLARE_SCALE_X] = row[GLARE_SCALE_Y] = 1.0f;
        for (int c = 0; c < 3; ++c)
            row[GLARE_GAIN_R + c] *=
                (float)(bright > 0 ? row[GLARE_STOP_COLORS + c] / bright : 0.0);
    }
}
