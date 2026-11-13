// MS_Glare's style: the elements of its JSON text as the rows the kernels read
// (halide/glare_layout.h). See glare_style.h.

#include "effects/glare_style.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "common/json.h"

#define PI 3.14159265358979323846
#define RADIANS (PI / 180.0)
// Lens Flare: the noise_amp texture's cells a radius per simplex_noise_freq, its change with
// the element's position (per S), the streaks' cells a radian per streaks_freq (fitted on the
// reference's correlation lengths)
#define TEXTURE_SCALE 1.95
#define TEXTURE_VARY 2.8
#define STREAK_SCALE 16.0

typedef struct {
    const MsJson *nodes;
    const MsGlareGlobals *g;
    float (*rows)[GLARE_FIELDS];
    int count, max;
    // Lens Flare: the lens's shadow element (-1: none); the last element's radius; while
    // adding a shadow row: its radius and centre.
    int shadow;
    double last_r;
    int as_shadow;
    double shadow_r, shadow_x, shadow_y;
    int images; // Lens Flare: the lens's "images" (-1: none)
    MsLensTextures *textures; // Lens Flare: the generated textures (NULL: none)
} Builder;

static double get(const Builder *b, int e, const char *key, double fallback) {
    return ms_json_get(b->nodes, e, key, fallback);
}

// Member `key` of `e` as numbers into `out` (at most `max`); returns how many.
static int get_list(const Builder *b, int e, const char *key, double *out, int max) {
    const int list = ms_json_member(b->nodes, e, key);
    int n = 0;
    if (list < 0 || b->nodes[list].type != MS_JSON_ARRAY)
        return 0;
    for (int c = b->nodes[list].first; c >= 0 && n < max; c = b->nodes[c].next)
        out[n++] = ms_json_number(b->nodes, c, 0.0);
    return n;
}

// MillSaph's random: a number in [0, 1) by (seed, i, k) (an integer hash; not the reference's).
static double random01(double seed, int i, int k) {
    uint32_t h = (uint32_t)(int32_t)floor(seed * 1000.0) * 0x9e3779b9u ^ (uint32_t)i * 0x85ebca6bu ^
                 (uint32_t)k * 0xc2b2ae35u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (h & 0xffffff) / 16777216.0;
}

static int is_type(const Builder *b, int e, const char *type) {
    return ms_json_is(b->nodes, e, "type", type);
}

// Lens Flare: a distance law's factor (scale s at distance d; 1 without one).
static double by_dist(const Builder *b, int e, double s, double d, const char *inverted,
                      const char *min_scale) {
    if (s == 0.0)
        return 1.0;
    const double f = get(b, e, inverted, 0.0) != 0.0 ? 1.0 - s * d : s * d;
    return fmax(f + get(b, e, min_scale, 0.0), 0.0);
}

// Lens Flare: element e's place on the axis from the pivot through the hotspot (its row's
// position and rotation), and its size, stretch and brightness factors there.
static void place(const Builder *b, int e, float *row, double *size_k, double *stretch_k,
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
    // change on a still (measured with the hotspot above left of the pivot)
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
    // offset_stretch_by_dist o: a blob scaled by 1 / sqrt(1 + o d); the others stretched
    // 1 + o d wider in their own frame, their area kept (12 polygons at 2 distances: exact)
    const double o = get(b, e, "offset_stretch_by_dist", 0.0);
    const int round_blob = is_type(b, e, "blob");
    const double shrink = round_blob ? 1.0 / sqrt(fmax(1.0 + o * d, 1e-6)) : 1.0;
    *stretch_k = round_blob ? 1.0 : 1.0 / fmax(1.0 + o * d, 1e-6);
    // rotate_to_pivot: turned along the line from the pivot through it, minus a half turn
    const double dx = px - g->pivot[0], dy = py - g->pivot[1];
    if (get(b, e, "rotate_to_pivot", 0.0) != 0.0)
        row[GLARE_ROTATE] += (float)((dx != 0 || dy != 0 ? atan2(dy, dx) : atan2(uy, ux)) - PI);
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
    // scale_brightness_by_dist s: x (min_brightness_scale + s d), or x (min + 1 - s d) when
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

// The row's gains times `gain` (per channel).
static void scale_gain(float *row, const double gain[3]) {
    for (int k = 0; k < 3; ++k)
        row[GLARE_GAIN_R + k] *= (float)gain[k];
}

// One element (not a container) into a new row.
static void add_element(Builder *b, int e) {
    if (b->count >= b->max)
        return;
    const MsGlareGlobals *g = b->g;
    float *row = b->rows[b->count];
    memset(row, 0, sizeof(float) * GLARE_FIELDS);
    // Lens Flare: placed first (its size and brightness factors there); Gamma g: each
    // element's brightness b -> b^(1 / g).
    // A shadow: its radius and centre given (its own size and place left out).
    double size_k = 1.0, stretch_k = 1.0, lens_gain[3] = {1, 1, 1};
    if (g->lens && !b->as_shadow)
        place(b, e, row, &size_k, &stretch_k, lens_gain);
    const double S = g->size;
    const double size =
        b->as_shadow ? b->shadow_r / (2.0 * S) : get(b, e, "size", 1.0) * size_k;
    if (b->as_shadow) {
        row[GLARE_POS_X] = (float)b->shadow_x;
        row[GLARE_POS_Y] = (float)b->shadow_y;
    }
    b->last_r = 2.0 * S * size;
    double bright = get(b, e, "brightness", 1.0);
    if (g->lens && !b->as_shadow && g->gamma > 0 && bright > 0)
        bright = pow(bright, 1.0 / g->gamma);
    double tint[3] = {1, 1, 1};
    get_list(b, e, "tint", tint, 3);
    // Glare: scaled by the distance to the pivot, 0 here (no offset), or mult_brightness_p:
    // nothing drawn (Lens Flare: drawn).
    if (!g->lens && (get(b, e, "scale_brightness_by_dist", 0.0) != 0.0 ||
                     get(b, e, "scale_width_by_dist", 0.0) != 0.0 ||
                     get(b, e, "mult_brightness_p", 0.0) != 0.0))
        return;
    const double rel_h = get(b, e, "rel_height", 1.0) * g->rel_height;
    const double rel_w = get(b, e, "rel_width", 1.0);
    // Stretch s: a blob scaled by 1 / sqrt(s); the others x / sqrt(s), y x sqrt(s) ("taller";
    // "wider" the other way).
    const double s = fmax(get(b, e, "stretch", 1.0) * stretch_k, 1e-6), stretch = 1.0 / sqrt(s);
    // (Lens Flare: always taller, whatever stretch_direction and stretch_offset_inverted;
    // measured on spots and polar flares at 3 rotations)
    const int wider = !g->lens && ms_json_is(b->nodes, e, "stretch_direction", "wider");
    const int blob = is_type(b, e, "blob");
    const double kx = blob ? stretch : wider ? sqrt(s) : stretch;
    const double ky = blob ? stretch : wider ? stretch : sqrt(s);
    row[GLARE_ROTATE] += (float)((get(b, e, "rotate", 0.0) + g->rotate) * RADIANS);
    // (Lens Flare: rel_width, rel_height in the screen's frame, whatever the rotation, the
    // stretch in the element's own; measured on Big Red's and Photonic's streaks: turned
    // rays and polygons squeezed flat)
    row[GLARE_SCALE_X] = (float)(g->lens ? kx : rel_w * kx);
    row[GLARE_SCALE_Y] = (float)(g->lens ? ky : rel_h * ky);
    row[GLARE_ANGLE_X] = (float)(g->lens ? 1.0 : rel_w);
    row[GLARE_ANGLE_Y] = (float)(g->lens ? 1.0 : rel_h);
    row[GLARE_REL_X] = (float)(g->lens ? rel_w : 0.0);
    row[GLARE_REL_Y] = (float)(g->lens ? rel_h : 0.0);
    // Lens Flare, noise_amp: the element x max(1 + 0.714 amp n, 0), n MillSaph's fbm (of
    // gradient noise, sd 0.28 an octave: the reference's sd 0.2 x amp at octaves_scale 0.2)
    // across the element, simplex_noise_freq x TEXTURE_SCALE cells a radius (x freq_rel_x,
    // freq_rel_y on the screen), octaves_scale x the amplitude an octave; changing as the
    // element moves x noise_vary_by_position (the reference's: half decorrelated at about 25 px
    // of travel at 1 for a radius of 44 px).
    const double amp = g->lens && !b->as_shadow ? get(b, e, "noise_amp", 0.0) : 0.0;
    if (amp != 0.0 && size > 0 && S > 0) {
        const double k = get(b, e, "simplex_noise_freq", 4.0) * TEXTURE_SCALE / (2.0 * S * size);
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

    if (is_type(b, e, "rays")) {
        // floor(ray_number / 2) lines; the length L = 2 S size x Rays Length x radius_range's
        // largest, cut at L unstretched; the width without Rays Length. Lens Flare: each line
        // its own length in radius_range, min + (max - min) sqrt(u) (u: MillSaph's random by
        // the line and rand_seed; the reference's mean energies at [0.5, 1], [0.2, 0.6]),
        // turned by jitter_angles x (u' - 0.5) x the lines' spacing (Glare: all at the largest)
        double color[3] = {1, 1, 1}, range[2] = {1, 1};
        get_list(b, e, "color", color, 3);
        get_list(b, e, "radius_range", range, 2);
        const double L0 = 2.0 * S * size, L = L0 * g->rays_length * range[1];
        const int lines = (int)floor(get(b, e, "ray_number", 4) * g->rays_num_scale / 2.0 + 1e-9);
        row[GLARE_KIND] = GLARE_RAYS;
        row[GLARE_R] = (float)L;
        row[GLARE_CLIP_R] = (float)L;
        row[GLARE_CLIP_SCALE] = 1.0f;
        row[GLARE_RAY_WIDTH] = (float)(get(b, e, "ray_width", 0.05) * L0 / 2.0 * g->rays_thickness);
        row[GLARE_LINES] = (float)(lines < GLARE_MAX_LINES ? lines : GLARE_MAX_LINES);
        row[GLARE_RAY_MIN] = (float)(g->lens && range[1] > 0 ? fmin(range[0] / range[1], 1.0) : 1.0);
        row[GLARE_RAY_SEED] = (float)get(b, e, "rand_seed", 0.0);
        row[GLARE_RAY_JITTER] = (float)(g->lens ? get(b, e, "jitter_angles", 0.0) : 0.0);
        // Lens Flare, streaks: thin stripes along the lines, x max(1 + amp (0.09 + 0.55 n), 0),
        // n MillSaph's fbm around a circle of streaks_freq x STREAK_SCALE cells' radius (the
        // reference's: mean +7 %, sd 0.11 x amp at freq 1, 0.23 at 4)
        const double streaks = g->lens ? get(b, e, "streaks_amp", 0.0) : 0.0;
        if (streaks != 0.0) {
            row[GLARE_STREAK_AMP] = (float)streaks;
            row[GLARE_STREAK_FREQ] = (float)(get(b, e, "streaks_freq", 1.0) * STREAK_SCALE);
            row[GLARE_STREAK_PERSIST] = (float)get(b, e, "streaks_octaves_scale", 0.3);
            row[GLARE_STREAK_SEED] = (float)(0.7 + 37.1 * get(b, e, "rand_seed", 0.0));
        }
        for (int c = 0; c < 3; ++c)
            row[GLARE_GAIN_R + c] = (float)(bright * color[c]);
        scale_gain(row, lens_gain);
        b->count += lines > 0;
        return;
    }

    double radii[GLARE_MAX_STOPS], colors[3 * GLARE_MAX_STOPS];
    int stops = get_list(b, e, "radii", radii, GLARE_MAX_STOPS);
    if (stops == 0)
        radii[0] = 1.0, stops = 1;
    int n_colors = get_list(b, e, "colors", colors, 3 * GLARE_MAX_STOPS) / 3;
    if (n_colors == 0)
        colors[0] = colors[1] = colors[2] = 1.0, n_colors = 1;
    for (int k = n_colors; k < stops; ++k) // too few colours: the last one repeated
        memcpy(&colors[3 * k], &colors[3 * (n_colors - 1)], 3 * sizeof(double));
    double R = 2.0 * S * size;
    const int polar = is_type(b, e, "polar_flare"), fancy = is_type(b, e, "fancy_spot");
    if (polar && !g->lens)
        R /= 1.0 + get(b, e, "shift_edge", 0.0);
    // (Lens Flare: shift_edge moves the element along its own x by that x its outer radius
    // (a polar flare: x 0.937), squeezed by rel_width, rel_height on the screen, its size
    // kept; measured on polar flares and a fancy spot, with and without rotate_to_pivot)
    if (g->lens && !b->as_shadow) {
        const double shift =
            get(b, e, "shift_edge", 0.0) * radii[stops - 1] * R * (polar ? 0.937 : 1.0);
        row[GLARE_POS_X] += (float)(shift * cos(row[GLARE_ROTATE]) * row[GLARE_REL_X]);
        row[GLARE_POS_Y] += (float)(shift * sin(row[GLARE_ROTATE]) * row[GLARE_REL_Y]);
    }
    row[GLARE_R] = (float)R;

    if (blob && stops == 1) {
        row[GLARE_KIND] = GLARE_FALLOFF;
        row[GLARE_FALLOFF_R] = (float)(1.977 * S * size * radii[0]);
        row[GLARE_CLIP_R] = (float)R;
        row[GLARE_CLIP_SCALE] = (float)stretch; // its circle: R x the stretch
        for (int c = 0; c < 3; ++c)
            row[GLARE_GAIN_R + c] = (float)(bright * tint[c] * colors[c]);
        scale_gain(row, lens_gain);
        ++b->count;
        return;
    }
    row[GLARE_KIND] = GLARE_STOPS;
    // (Lens Flare: the edges of every element with stops as a pixel's coverage, fitted on a
    // spot and a polar flare)
    if (g->lens)
        row[GLARE_CRISP] = 2.0f;
    row[GLARE_STOP_COUNT] = (float)stops;
    for (int k = 0; k < stops; ++k) {
        row[GLARE_STOP_RADII + k] = (float)radii[k];
        for (int c = 0; c < 3; ++c)
            row[GLARE_STOP_COLORS + 3 * k + c] = (float)colors[3 * k + c];
    }
    for (int c = 0; c < 3; ++c)
        row[GLARE_GAIN_R + c] = (float)(bright * tint[c]);
    row[GLARE_FADE] = (float)get(b, e, "fade_gradient", 0.0);
    row[GLARE_BOOST] = 1.0f;
    row[GLARE_RW_MIN] = row[GLARE_RW_MAX] = 1.0f;
    if (polar || fancy) {
        row[GLARE_SECTOR] = 1.0f;
        row[GLARE_START] = (float)((180.0 + get(b, e, "start_angle", 0.0)) * RADIANS);
        row[GLARE_WIDTH] = (float)(fmin(get(b, e, "angle_width", 360.0), 360.0) * RADIANS);
        row[GLARE_SOFTNESS] = (float)get(b, e, "arc_edge_softness", 0.0);
        row[GLARE_REPEATS] = (float)fmax(1.0, floor(get(b, e, "repeats", 1.0)));
        const int n_sides = (int)get(b, e, "n_sides", 0.0);
        const int sides = n_sides < 3 ? 0 : n_sides < GLARE_MAX_SIDES ? n_sides : GLARE_MAX_SIDES;
        row[GLARE_SIDES] = (float)sides;
        // the vertices: on the circle, the first at the start angle; random_side_lengths r:
        // each side's angle (1 - r) x its share + r x a random share (MillSaph's random by
        // rand_seed, 0.05 + u each; the reference's: the vertices on the circle, the first one
        // kept, energy -12 to -18 % at r 1 on a hexagon, -3 to -5 % at 0.5)
        double r = g->lens ? fmin(fmax(get(b, e, "random_side_lengths", 0.0), 0.0), 1.0) : 0.0;
        const double seed = get(b, e, "rand_seed", 0.0);
        double weights[GLARE_MAX_SIDES], total = 0.0, largest = 0.0;
        for (int k = 0; k < sides; ++k) {
            total += weights[k] = 0.05 + random01(seed, k, 7);
            largest = fmax(largest, weights[k]);
        }
        // (no side over 0.95 pi: the centre inside)
        const double over = sides > 0 ? largest / total - 1.0 / sides : 0.0;
        if (over > 0 && r * over > 0.475 - 1.0 / sides)
            r = (0.475 - 1.0 / sides) / over;
        row[GLARE_IRREGULAR] = (float)(sides > 0 && r > 0);
        double at = 0.0;
        for (int k = 0; k <= GLARE_MAX_SIDES; ++k) {
            row[GLARE_VERTICES + k] = (float)(k < sides ? at : k == sides ? 2.0 * PI : 7.0);
            if (k < sides)
                at += 2.0 * PI * ((1.0 - r) / sides + r * weights[k] / total);
        }
        row[GLARE_ROUNDNESS] = (float)get(b, e, "roundness", 0.0);
        // Blur Gradient: the radial profile blurred, sigma blur_gradient x R / 2.8 (with the
        // edges' antialiasing, 0.6 px)
        const double blur = get(b, e, "blur_gradient", 0.0) * R / 2.8;
        row[GLARE_SIGMA] = (float)(blur > 0 ? sqrt(blur * blur + 0.36) : 0.0);
    }
    if (polar && g->lens && (get(b, e, "r_noise_min", 1.0) != 1.0 ||
                             get(b, e, "r_noise_max", 1.0) != 1.0)) {
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
    if (polar && g->lens && get(b, e, "noise_brightness", 0.0) != 0.0) {
        // noise_brightness: x it x MillSaph's noise around the element (as the radius
        // noise's: 0.5 + 0.4 x perlin, sd 0.1, fitted on the reference's means over 7
        // widths and softnesses), thresholded by noise_width and noise_softness as the
        // waves, averaged over blur_theta_map x a turn
        const int octaves = (int)fmin(fmax(get(b, e, "noise_octaves", 1.0), 1.0), 8.0);
        double total = 0.0;
        for (int k = 0; k < octaves; ++k)
            total += pow(0.43, k);
        row[GLARE_NB] = (float)get(b, e, "noise_brightness", 0.0);
        row[GLARE_NB_WIDTH] = (float)get(b, e, "noise_width", 0.3);
        row[GLARE_NB_SOFT] = (float)get(b, e, "noise_softness", 1.0);
        row[GLARE_NB_RHO] = (float)(0.529 * pow(fmax(get(b, e, "noise_freq", 120.0), 0.0), 0.785));
        row[GLARE_NB_SEED] = (float)(0.7 + 37.1 * get(b, e, "rand_seed", 0.0));
        row[GLARE_NB_OCTAVES] = (float)octaves;
        row[GLARE_NB_SCALE] = (float)(0.4 / total);
        row[GLARE_NB_BLUR] = (float)fmin(fmax(get(b, e, "blur_theta_map", 0.0), 0.0), 1.0);
    }
    if (polar) {
        if (!g->lens)
            row[GLARE_CRISP] = 1.0f;
        row[GLARE_BOOST] = (float)(1.0 + get(b, e, "offset_bright", 0.0));
        row[GLARE_WAVE] = (float)get(b, e, "wave_brightness", 0.0);
        row[GLARE_WAVE_FREQ] = (float)get(b, e, "wave_freq", 1.0);
        row[GLARE_WAVE_WIDTH] = (float)get(b, e, "wave_width", 1.0);
        // (Lens Flare: x wave_brightness, thresholded by width and softness, shifted by
        // wave_phase)
        row[GLARE_WAVE_LENS] = (float)g->lens;
        row[GLARE_WAVE_SOFT] = (float)get(b, e, "wave_softness", 1.0);
        row[GLARE_WAVE_PHASE] = (float)(g->lens ? get(b, e, "wave_phase", 0.0) : 0.0);
        row[GLARE_RW_MIN] = (float)get(b, e, "r_wave_min", 1.0);
        row[GLARE_RW_MAX] = (float)get(b, e, "r_wave_max", 1.0);
        row[GLARE_RW_FREQ] = (float)get(b, e, "r_wave_freq", 1.0);
        row[GLARE_RW_SPIKE] = (float)get(b, e, "r_spikyness", 1.0);
    }
    scale_gain(row, lens_gain);
    ++b->count;
}

// Lens Flare: element e (just added, row `owner`) masked by the lens's shadow when it asks
// (use_shadow_p): a row of the shadow element of radius R x shadow_rel_scale (R: the
// element's), centred on the element moved by R x (amount max(|amount| / 2, 1)^0.09 x law -
// shadow_offset_along_axis) towards the pivot and R x shadow_offset_off_axis across. The law,
// by D (the hotspot's distance to the pivot, in units) and the delay (shadow_animation_delay):
// none up to D = 5 delay, then (0.29 + 0.19 delay) (D - 5 delay)^(1.29 + 1.5 delay); at delay
// 0, 0.2375 D^1.06 (amount: shadow_amount_at_edge; fitted on D 0.25 to 2.8, delays 0 to 1,
// within 0.02 R at amount 2; amounts -7 to 7, any centre, at delay 0.1).
static void add_shadow(Builder *b, int e, int owner) {
    const MsGlareGlobals *g = b->g;
    if (b->shadow < 0 || get(b, e, "use_shadow_p", 0.0) == 0.0 || b->count >= b->max ||
        !(g->size > 0))
        return;
    const double R = b->last_r;
    const double ax = g->pivot[0] - g->hotspot[0], ay = g->pivot[1] - g->hotspot[1];
    const double length = sqrt(ax * ax + ay * ay);
    const double ux = length > 0 ? ax / length : 1.0, uy = length > 0 ? ay / length : 0.0;
    // shadow_dist_calc "use element": D the element's distance to the pivot, not the hotspot's
    const double d =
        ms_json_is(b->nodes, e, "shadow_dist_calc", "use element")
            ? hypot(b->rows[owner][GLARE_POS_X] - g->pivot[0],
                    b->rows[owner][GLARE_POS_Y] - g->pivot[1]) / g->unit
            : length / g->unit;
    const double delay = fmax(get(b, e, "shadow_animation_delay", 0.1), 0.0);
    const double past = fmax(d - 5.0 * delay, 0.0);
    const double law = delay > 0 ? (0.29 + 0.19 * delay) * pow(past, 1.29 + 1.5 * delay)
                                 : 0.2375 * pow(past, 1.06);
    const double amount = get(b, e, "shadow_amount_at_edge", 1.0);
    const double along = amount * pow(fmax(fabs(amount) / 2.0, 1.0), 0.09) * law -
                         get(b, e, "shadow_offset_along_axis", 0.0);
    const double across = get(b, e, "shadow_offset_off_axis", 0.0);
    float *row = b->rows[owner];
    b->as_shadow = 1;
    b->shadow_r = R * get(b, e, "shadow_rel_scale", 1.0);
    b->shadow_x = row[GLARE_POS_X] + R * (along * ux - across * uy);
    b->shadow_y = row[GLARE_POS_Y] + R * (along * uy + across * ux);
    const int at = b->count;
    add_element(b, b->shadow);
    b->as_shadow = 0;
    if (b->count == at)
        return;
    b->rows[at][GLARE_HIDDEN] = 1.0f;
    row[GLARE_SHADOW_ROW] = (float)(at + 1);
    row[GLARE_SHADOW_INVERT] = (float)(get(b, e, "invert_shadow", 0.0) != 0.0);
    row[GLARE_SHADOW_OPACITY] = (float)(1.0 - get(b, e, "shadow_transparency", 0.0));
}

// Lens Flare: image k of the lens (`images`, an array): its size and channels, and where its
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

static size_t lens_texel_count(const MsJson *nodes) {
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

// Lens Flare: an image written in the element itself (its "image" with "pixels", "xdim",
// "ydim", "elnum"), added after the lens's own images; its size and first texel.
static int written(Builder *b, int image, int *w, int *h, size_t *start) {
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

// Lens Flare: one of MillSaph's own textures, as `image` ({"generate": ...}) asks: added to
// the ones to generate (the same settings: the same texture); its size and first texel.
static int generated(Builder *b, int image, int *w, int *h, size_t *start) {
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
                                t->texels,
                                -1,
                                3};
    for (int i = 0; i < t->count; ++i) {
        const MsLensTexture *have = &t->list[i];
        if (have->kind == want.kind && have->seed == want.seed && have->amount == want.amount &&
            have->scale == want.scale && have->detail == want.detail &&
            have->sides == want.sides && have->width == want.width) {
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

// Lens Flare: element e's image (its "image": {"save_index": k} or {"generate": ...}) on its
// row (just added):
// where it is and how big (fitted on MillSaph's test images at 512 x 512, exact maps): the
// side that fits (the shorter one; a texture: the longer) 2 (R + 1.5) px x
// image_scale_size (image_absolute_size_p: the frame's width; a texture: 2 R, no image_*
// setting), its columns turned by rotate + image_offset_rotate (+ the angle from the pivot
// with rotate_to_pivot; image_absolute_rotate_p: image_offset_rotate only), its rows a
// quarter turn clockwise from them, then stretched as the element (in the element's frame),
// centred on the element (image_absolute_position_p: the pivot) moved by image_offset_x, y
// x half that side. A texture: the image x its colours (not its brightness).
static void add_image(Builder *b, int e, float *row) {
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
    const double rot = row[GLARE_ROTATE];
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

// The elements under `list` (an array), containers opened.
static void add_list(Builder *b, int list, int depth) {
    if (list < 0 || b->nodes[list].type != MS_JSON_ARRAY || depth > 32)
        return;
    for (int e = b->nodes[list].first; e >= 0; e = b->nodes[e].next) {
        if (b->nodes[e].type != MS_JSON_OBJECT || get(b, e, "disabled", 0.0) != 0.0)
            continue;
        if (is_type(b, e, "container"))
            add_list(b, ms_json_member(b->nodes, e, "children"), depth + 1);
        else {
            const int at = b->count;
            add_element(b, e);
            if (b->g->lens && b->count > at) {
                add_image(b, e, b->rows[at]);
                add_shadow(b, e, at);
            }
        }
    }
}

// The first element (not a container) under `list`, or -1.
static int first_element(const Builder *b, int list, int depth) {
    if (list < 0 || b->nodes[list].type != MS_JSON_ARRAY || depth > 32)
        return -1;
    for (int e = b->nodes[list].first; e >= 0; e = b->nodes[e].next) {
        if (b->nodes[e].type != MS_JSON_OBJECT)
            continue;
        if (!is_type(b, e, "container"))
            return e;
        const int inner = first_element(b, ms_json_member(b->nodes, e, "children"), depth + 1);
        if (inner >= 0)
            return inner;
    }
    return -1;
}

int ms_glare_style(const char *text, MsJson *nodes, int max_nodes, const MsGlareGlobals *g,
                   float (*rows)[GLARE_FIELDS], int max_rows, MsLensTextures *textures) {
    // The text may start with other characters (the reference's 32 dashes): from the first {.
    const char *start = text ? strchr(text, '{') : NULL;
    if (!start)
        return -1;
    const int count = ms_json_parse(start, strlen(start), nodes, max_nodes);
    if (count <= 0)
        return -1;
    // {"value": {"elements": [...]}} as the reference writes it, or {"elements": [...]}.
    int root = 0;
    const int value = ms_json_member(nodes, 0, "value");
    if (value >= 0)
        root = value;
    Builder b = {nodes, g, rows, 0, max_rows, -1, 0.0, 0, 0.0, 0.0, 0.0, -1, NULL};
    b.shadow = g->lens ? first_element(&b, ms_json_member(nodes, root, "shadow"), 0) : -1;
    b.images = g->lens ? ms_json_member(nodes, root, "images") : -1;
    if (textures) { // the generated ones after the lens's own images
        textures->count = 0;
        textures->texels = g->lens ? lens_texel_count(nodes) : 0;
        b.textures = textures;
    }
    add_list(&b, ms_json_member(nodes, root, "elements"), 0);
    return b.count;
}
