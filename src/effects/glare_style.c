// MS_Glare's style: the elements of its JSON text as the rows the kernels read
// (halide/glare_layout.h). See glare_style.h.

#include "effects/glare_style.h"

#include <math.h>
#include <string.h>

#include "common/json.h"

#define PI 3.14159265358979323846
#define RADIANS (PI / 180.0)

typedef struct {
    const MsJson *nodes;
    const MsGlareGlobals *g;
    float (*rows)[GLARE_FIELDS];
    int count, max;
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

static int is_type(const Builder *b, int e, const char *type) {
    return ms_json_is(b->nodes, e, "type", type);
}

// One element (not a container) into a new row.
static void add_element(Builder *b, int e) {
    if (b->count >= b->max)
        return;
    const MsGlareGlobals *g = b->g;
    float *row = b->rows[b->count];
    memset(row, 0, sizeof(float) * GLARE_FIELDS);
    const double S = g->size, size = get(b, e, "size", 1.0);
    const double bright = get(b, e, "brightness", 1.0);
    double tint[3] = {1, 1, 1};
    get_list(b, e, "tint", tint, 3);
    // Scaled by the distance to the frame's centre (the elements here have no offset), or
    // mult_brightness_p: nothing drawn.
    if (get(b, e, "scale_brightness_by_dist", 0.0) != 0.0 ||
        get(b, e, "scale_width_by_dist", 0.0) != 0.0 || get(b, e, "mult_brightness_p", 0.0) != 0.0)
        return;
    const double rel_h = get(b, e, "rel_height", 1.0) * g->rel_height;
    const double rel_w = get(b, e, "rel_width", 1.0);
    // Stretch s: a blob scaled by 1 / sqrt(s); the others x / sqrt(s), y x sqrt(s) ("taller";
    // "wider" the other way).
    const double s = fmax(get(b, e, "stretch", 1.0), 1e-6), stretch = 1.0 / sqrt(s);
    const int wider = ms_json_is(b->nodes, e, "stretch_direction", "wider");
    const int blob = is_type(b, e, "blob");
    const double kx = blob ? stretch : wider ? sqrt(s) : stretch;
    const double ky = blob ? stretch : wider ? stretch : sqrt(s);
    row[GLARE_ROTATE] = (float)((get(b, e, "rotate", 0.0) + g->rotate) * RADIANS);
    row[GLARE_SCALE_X] = (float)(rel_w * kx);
    row[GLARE_SCALE_Y] = (float)(rel_h * ky);
    row[GLARE_ANGLE_X] = (float)rel_w;
    row[GLARE_ANGLE_Y] = (float)rel_h;

    if (is_type(b, e, "rays")) {
        // floor(ray_number / 2) lines; the length L = 2 S size x Rays Length, drawn at its
        // radius_range's largest (the reference draws each ray's length in that range), cut
        // at L unstretched; the width without Rays Length
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
        for (int c = 0; c < 3; ++c)
            row[GLARE_GAIN_R + c] = (float)(bright * color[c]);
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
    if (polar)
        R /= 1.0 + get(b, e, "shift_edge", 0.0);
    row[GLARE_R] = (float)R;

    if (blob && stops == 1) {
        row[GLARE_KIND] = GLARE_FALLOFF;
        row[GLARE_FALLOFF_R] = (float)(1.977 * S * size * radii[0]);
        row[GLARE_CLIP_R] = (float)R;
        row[GLARE_CLIP_SCALE] = (float)stretch; // its circle: R x the stretch
        for (int c = 0; c < 3; ++c)
            row[GLARE_GAIN_R + c] = (float)(bright * tint[c] * colors[c]);
        ++b->count;
        return;
    }
    row[GLARE_KIND] = GLARE_STOPS;
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
        const int sides = (int)get(b, e, "n_sides", 0.0);
        row[GLARE_SIDES] = (float)(sides >= 3 ? sides : 0);
        row[GLARE_ROUNDNESS] = (float)get(b, e, "roundness", 0.0);
        // Blur Gradient: the radial profile blurred, sigma blur_gradient x R / 2.8 (with the
        // edges' antialiasing, 0.6 px)
        const double blur = get(b, e, "blur_gradient", 0.0) * R / 2.8;
        row[GLARE_SIGMA] = (float)(blur > 0 ? sqrt(blur * blur + 0.36) : 0.0);
    }
    if (polar) {
        row[GLARE_CRISP] = 1.0f;
        row[GLARE_BOOST] = (float)(1.0 + get(b, e, "offset_bright", 0.0));
        row[GLARE_WAVE] = (float)get(b, e, "wave_brightness", 0.0);
        row[GLARE_WAVE_FREQ] = (float)get(b, e, "wave_freq", 1.0);
        row[GLARE_WAVE_WIDTH] = (float)get(b, e, "wave_width", 1.0);
        row[GLARE_RW_MIN] = (float)get(b, e, "r_wave_min", 1.0);
        row[GLARE_RW_MAX] = (float)get(b, e, "r_wave_max", 1.0);
        row[GLARE_RW_FREQ] = (float)get(b, e, "r_wave_freq", 1.0);
        row[GLARE_RW_SPIKE] = (float)get(b, e, "r_spikyness", 1.0);
    }
    ++b->count;
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
        else
            add_element(b, e);
    }
}

int ms_glare_style(const char *text, MsJson *nodes, int max_nodes, const MsGlareGlobals *g,
                   float (*rows)[GLARE_FIELDS], int max_rows) {
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
    Builder b = {nodes, g, rows, 0, max_rows};
    add_list(&b, ms_json_member(nodes, root, "elements"), 0);
    return b.count;
}
