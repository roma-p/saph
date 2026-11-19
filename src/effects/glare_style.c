// MS_Glare's style and MS_LensFlare's lens: the elements of their JSON text as the rows the
// kernels read (halide/glare_layout.h). See glare_style.h; what only the Lens Flare's
// elements have: lensflare_style.c.

#include "effects/glare_style.h"

#include <string.h>

#include "effects/glare_build.h"

// The row's gains times `gain` (per channel).
static void scale_gain(float *row, const double gain[3]) {
    for (int k = 0; k < 3; ++k)
        row[GLARE_GAIN_R + k] *= (float)gain[k];
}

// A rays element (its size `size` x S pixels; `bright`: its brightness, `gain` the Lens
// Flare's factors there): floor(ray_number / 2) lines; the length L = 2 S size x Rays Length x
// radius_range's largest, cut at L unstretched; the width without Rays Length. Lens Flare:
// each line its own length in radius_range, min + (max - min) sqrt(u) (u: MillSaph's random by
// the line and rand_seed; the reference's mean energies at [0.5, 1], [0.2, 0.6]), turned by
// jitter_angles x (u' - 0.5) x the lines' spacing (Glare: all at the largest).
static void add_rays(GlareBuilder *b, int e, float *row, double size, double bright,
                     const double gain[3]) {
    const MsGlareGlobals *g = b->g;
    const double S = g->size;
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
    if (g->lens)
        ms_lens_streaks(b, e, row);
    for (int c = 0; c < 3; ++c)
        row[GLARE_GAIN_R + c] = (float)(bright * color[c]);
    scale_gain(row, gain);
    b->count += lines > 0;
}

// A polar flare's or fancy spot's sector and polygon (radius R pixels).
static void add_sector(const GlareBuilder *b, int e, float *row, double R) {
    const MsGlareGlobals *g = b->g;
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

// A polar flare's waves (angular, of the radius).
static void add_waves(const GlareBuilder *b, int e, float *row) {
    const MsGlareGlobals *g = b->g;
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

// An element with colour stops (a blob of one stop: its soft falloff), its size `size` x S
// pixels, `stretch` its stretch's scale; `bright`, `tint`: its brightness and tint, `gain`
// the Lens Flare's factors there.
static void add_stops(GlareBuilder *b, int e, float *row, double size, double stretch,
                      double bright, const double tint[3], const double gain[3]) {
    const MsGlareGlobals *g = b->g;
    const double S = g->size;
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
    if (g->lens && !b->as_shadow)
        ms_lens_shift_edge(b, e, row, radii[stops - 1], R, polar);
    row[GLARE_R] = (float)R;

    if (is_type(b, e, "blob") && stops == 1) {
        row[GLARE_KIND] = GLARE_FALLOFF;
        row[GLARE_FALLOFF_R] = (float)(1.977 * S * size * radii[0]);
        row[GLARE_CLIP_R] = (float)R;
        row[GLARE_CLIP_SCALE] = (float)stretch; // its circle: R x the stretch
        for (int c = 0; c < 3; ++c)
            row[GLARE_GAIN_R + c] = (float)(bright * tint[c] * colors[c]);
        scale_gain(row, gain);
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
    if (polar || fancy)
        add_sector(b, e, row, R);
    if (polar) // (Glare's too: its styles of 2026 use them)
        ms_lens_polar_noises(b, e, row);
    if (polar)
        add_waves(b, e, row);
    scale_gain(row, gain);
    ++b->count;
}

void ms_style_add_element(GlareBuilder *b, int e) {
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
        ms_lens_place(b, e, row, &size_k, &stretch_k, lens_gain);
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
    // (Glare's renderer 8 styles, but rays: always taller, the stretch along the element's
    // rotation; measured on a fancy spot at 0, 30, 90 degrees, either direction)
    const int spun = b->turned && !is_type(b, e, "rays");
    const int wider = !g->lens && !spun && ms_json_is(b->nodes, e, "stretch_direction", "wider");
    const int blob = is_type(b, e, "blob");
    const double kx = blob ? stretch : wider ? sqrt(s) : stretch;
    const double ky = blob ? stretch : wider ? stretch : sqrt(s);
    row[GLARE_ROTATE] += (float)((get(b, e, "rotate", 0.0) + g->rotate) * RADIANS);
    // (Lens Flare: rel_width, rel_height in the screen's frame, whatever the rotation, the
    // stretch in the element's own; measured on Big Red's and Photonic's streaks: turned
    // rays and polygons squeezed flat)
    if (spun)
        row[GLARE_TURNED_FRAME] = 2.0f;
    row[GLARE_SCALE_X] = (float)(g->lens ? kx : rel_w * kx);
    row[GLARE_SCALE_Y] = (float)(g->lens ? ky : rel_h * ky);
    row[GLARE_ANGLE_X] = (float)(g->lens ? 1.0 : rel_w);
    row[GLARE_ANGLE_Y] = (float)(g->lens ? 1.0 : rel_h);
    row[GLARE_REL_X] = (float)(g->lens ? rel_w : 0.0);
    row[GLARE_REL_Y] = (float)(g->lens ? rel_h : 0.0);
    if (g->lens && !b->as_shadow && S > 0)
        ms_lens_texture_noise(b, e, row, 2.0 * S * size);
    if (is_type(b, e, "rays"))
        add_rays(b, e, row, size, bright, lens_gain);
    else
        add_stops(b, e, row, size, stretch, bright, tint, lens_gain);
}

// The elements under `list` (an array), containers opened.
static void add_list(GlareBuilder *b, int list, int depth) {
    if (list < 0 || b->nodes[list].type != MS_JSON_ARRAY || depth > 32)
        return;
    for (int e = b->nodes[list].first; e >= 0; e = b->nodes[e].next) {
        if (b->nodes[e].type != MS_JSON_OBJECT || get(b, e, "disabled", 0.0) != 0.0)
            continue;
        if (is_type(b, e, "container"))
            add_list(b, ms_json_member(b->nodes, e, "children"), depth + 1);
        else {
            const int at = b->count;
            ms_style_add_element(b, e);
            if (b->g->lens && b->count > at) {
                ms_lens_image(b, e, b->rows[at]);
                ms_lens_shadow(b, e, at);
            }
        }
    }
}

// The first element (not a container) under `list`, or -1.
static int first_element(const GlareBuilder *b, int list, int depth) {
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
    GlareBuilder b = {nodes, g, rows, 0, max_rows, -1, 0.0, 0, 0.0, 0.0, 0.0, -1, NULL, 0};
    b.shadow = g->lens ? first_element(&b, ms_json_member(nodes, root, "shadow"), 0) : -1;
    b.turned = !g->lens && ms_json_get(nodes, root, "renderer_version", 0.0) >= 8.0;
    b.images = g->lens ? ms_json_member(nodes, root, "images") : -1;
    if (textures) { // the generated ones after the lens's own images
        textures->count = 0;
        textures->texels = g->lens ? ms_lens_texel_count(nodes) : 0;
        b.textures = textures;
    }
    add_list(&b, ms_json_member(nodes, root, "elements"), 0);
    return b.count;
}
