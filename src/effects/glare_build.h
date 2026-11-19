// The builder of the style's rows, shared by glare_style.c (MS_Glare's and MS_LensFlare's
// elements) and lensflare_style.c (what only the Lens Flare's elements have: their place on
// the axis, shadows, images, noises). Not a public interface: see glare_style.h.

#pragma once

#include <math.h>
#include <stdint.h>

#include "common/json.h"
#include "effects/glare_style.h"

#define PI 3.14159265358979323846
#define RADIANS (PI / 180.0)

typedef struct GlareBuilder {
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
    // Glare: a style of the reference's renderer 8 or later ("renderer_version"): the elements'
    // stretch (but the rays') along their rotation (GLARE_TURNED_FRAME 2)
    int turned;
} GlareBuilder;

static inline double get(const GlareBuilder *b, int e, const char *key, double fallback) {
    return ms_json_get(b->nodes, e, key, fallback);
}

// Member `key` of `e` as numbers into `out` (at most `max`); returns how many.
static inline int get_list(const GlareBuilder *b, int e, const char *key, double *out, int max) {
    const int list = ms_json_member(b->nodes, e, key);
    int n = 0;
    if (list < 0 || b->nodes[list].type != MS_JSON_ARRAY)
        return 0;
    for (int c = b->nodes[list].first; c >= 0 && n < max; c = b->nodes[c].next)
        out[n++] = ms_json_number(b->nodes, c, 0.0);
    return n;
}

static inline int is_type(const GlareBuilder *b, int e, const char *type) {
    return ms_json_is(b->nodes, e, "type", type);
}

// MillSaph's random: a number in [0, 1) by (seed, i, k) (an integer hash; not the reference's).
static inline double random01(double seed, int i, int k) {
    uint32_t h = (uint32_t)(int32_t)floor(seed * 1000.0) * 0x9e3779b9u ^ (uint32_t)i * 0x85ebca6bu ^
                 (uint32_t)k * 0xc2b2ae35u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (h & 0xffffff) / 16777216.0;
}

// glare_style.c: element e (not a container) into a new row.
void ms_style_add_element(GlareBuilder *b, int e);

// lensflare_style.c, the Lens Flare's elements only:
// element e's place on the axis (its row's position and rotation), its size factor, what
// its stretch and the stretch's inverse gain, and its brightness factors there;
void ms_lens_place(const GlareBuilder *b, int e, float *row, double *size_k, double stretch_add[2],
                   double gain[3]);
// its noise_amp texture (`radius`: its radius in pixels);
void ms_lens_texture_noise(const GlareBuilder *b, int e, float *row, double radius);
// a rays element's streaks;
void ms_lens_streaks(const GlareBuilder *b, int e, float *row);
// shift_edge (`stop`: the last stop's radius, a fraction of `R`) and, for a polar flare, its
// radius noise and noise_brightness;
void ms_lens_shift_edge(const GlareBuilder *b, int e, float *row, double stop, double R,
                        int polar);
void ms_lens_polar_noises(const GlareBuilder *b, int e, float *row);
// its image and its shadow, once its row (`owner`) is added;
void ms_lens_image(GlareBuilder *b, int e, float *row);
void ms_lens_shadow(GlareBuilder *b, int e, int owner);
// the texels of the lens's own images.
size_t ms_lens_texel_count(const MsJson *nodes);
