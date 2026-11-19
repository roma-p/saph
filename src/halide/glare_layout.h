// MS_Glare's style as the kernels get it: one row of GLARE_FIELDS floats per element (the
// elements that draw, the containers flattened), built by effects/glare_style.c from the
// style's text and read by src/halide/glare_generator.cpp. Shared by C and C++.

#pragma once

#define GLARE_MAX_ELEMENTS 256
// Lens Flare's rows: the elements at up to 16 times of a motion blur (a constant size: the
// kernel faster than with a size of its own each render)
#define GLARE_MAX_ROWS (16 * GLARE_MAX_ELEMENTS)
#define GLARE_MAX_STOPS 32 // colour stops of an element
#define GLARE_MAX_LINES 64 // ray lines of a rays element
#define GLARE_MAX_SIDES 16 // sides of a polygon

// An element's kind (GLARE_KIND).
enum {
    GLARE_FALLOFF = 0, // one colour stop: the soft blob falloff
    GLARE_STOPS = 1,   // colour stops along the radius (spot, ring, blob, polar_flare, fancy_spot)
    GLARE_RAYS = 2,    // ray lines through the centre
    GLARE_IMAGE = 3,   // Lens Flare's texture: its image alone (GLARE_TEX_*)
};

// Lens Flare: how a row is drawn (ms_lensflare_draw sums each list apart: the shadow costs
// another element's value, the noises and images their own work).
enum {
    GLARE_DRAW_PLAIN = 0,
    GLARE_DRAW_SHADOWED = 1,         // masked by its shadow
    GLARE_DRAW_SPECIAL = 2,          // with noises or an image
    GLARE_DRAW_SPECIAL_SHADOWED = 3, // both
    GLARE_DRAW_LISTS = 4,
};

// Lens Flare's generated textures (an element's "image": {"generate": ...}).
enum {
    GLARE_TEXTURE_DIRT = 0,  // lens dirt: dust specks and smudges
    GLARE_TEXTURE_GLINT = 1, // a prism glint: a rainbow streak
    GLARE_TEXTURE_RAMP = 2,  // a rainbow ramp, radial or across
    GLARE_TEXTURE_IRIS = 3,  // an iris: a polygon of blades with fine streaks and ripples
};

// Lens Flare's images: their texels (RGB floats, row 0 first, one image after the other)
// in an image GLARE_TEXEL_ROW floats wide.
#define GLARE_TEXEL_ROW 4096

// The fields of a row (sizes, lengths in pixels of the render; angles in radians).
enum {
    GLARE_KIND,
    GLARE_R,          // the gradient's radius (stops) or the ray length (rays)
    GLARE_FALLOFF_R,  // FALLOFF: the profile's radius
    GLARE_GAIN_R,     // brightness x tint (x colour for FALLOFF and RAYS), per channel
    GLARE_GAIN_G,
    GLARE_GAIN_B,
    GLARE_SCALE_X,    // the element frame: x / (rel_width x stretch), y / (rel_height x ...)
    GLARE_SCALE_Y,
    GLARE_ANGLE_X,    // the frame of the angles (sectors, waves) and of the rays' width:
    GLARE_ANGLE_Y,    // x / rel_width, y / rel_height (no stretch)
    GLARE_ROTATE,     // its rotation
    GLARE_CLIP_R,     // FALLOFF, RAYS: nothing beyond this radius of the offset / CLIP_SCALE
    GLARE_CLIP_SCALE, // FALLOFF: the stretch's scale (RAYS: 1)
    GLARE_SECTOR,     // 1: an angular sector (polar_flare, fancy_spot)
    GLARE_CRISP,      // 1: the band's edges over one pixel (polar_flare), else over ~2; 2 (Lens
                      // Flare): a ramp of 1.18 px centred 0.21 px out
    GLARE_POS_X,      // Lens Flare: the element's centre (pixels, y up; Glare: 0)
    GLARE_POS_Y,
    GLARE_SHADOW_ROW,     // Lens Flare: 1 + the row of the shadow that masks it (0: none)
    GLARE_SHADOW_INVERT,  // 1: masked by 1 - the shadow
    GLARE_SHADOW_OPACITY, // 1 - shadow_transparency
    GLARE_HIDDEN,         // 1: a shadow's row, not drawn
    GLARE_TURNED_FRAME,   // 1 (Lens Flare): stretched in its own turned frame (Glare: along x,
                          // y; 2: Glare's renderer 8, the stretch alone turned)
    GLARE_ATMOSPHERE,     // 1 (Lens Flare): x the Atmosphere (not ignore_atmosphere)
    GLARE_RN_OCTAVES,     // Lens Flare, radius noise (r_noise): octaves (0: none)
    GLARE_RN_MIN,         // its radius scale at the noise's 0 and 1
    GLARE_RN_MAX,
    GLARE_RN_RHO,         // the radius of the circle the noise is read on (its frequency)
    GLARE_RN_SEED,        // the noise's third coordinate
    GLARE_RN_SCALE,       // the noise 0.47 + this x its octaves' sum (about 0..1)
    GLARE_WAVE_LENS,      // 1 (Lens Flare): the waves x wave_brightness, by width and softness
    GLARE_WAVE_SOFT,      // wave_softness
    GLARE_WAVE_PHASE,     // wave_phase
    GLARE_NB,             // Lens Flare, noise_brightness: x this x the thresholded noise (0: none)
    GLARE_NB_WIDTH,       // noise_width, noise_softness: the threshold, as the waves'
    GLARE_NB_SOFT,
    GLARE_NB_RHO,         // as GLARE_RN_*: the circle's radius, the third coordinate, octaves,
    GLARE_NB_SEED,        // the scale of the octaves' sum (0.5 + this x the sum)
    GLARE_NB_OCTAVES,
    GLARE_NB_SCALE,
    GLARE_NB_BLUR,        // blur_theta_map: the thresholded noise averaged over this x a turn
    GLARE_NOISE_SLOT,     // Lens Flare: the row of the noise rows with its noises (the rows
                          // of a motion blur's times share them)
    GLARE_TN_AMP,         // Lens Flare, noise_amp: x max(1 + this x a fbm, 0) (0: none), the
    GLARE_TN_FX,          // fbm at (dx x FX, dy x FY, Z) (pixels from the centre), octaves
    GLARE_TN_FY,          // PERSIST^k
    GLARE_TN_Z,           // (Z and U: where it is read, by the element's position)
    GLARE_TN_U,
    GLARE_TN_PERSIST,
    GLARE_STREAK_AMP,     // Lens Flare, RAYS' streaks: each line x max(1 + AMP (mean + k n), 0),
    GLARE_STREAK_FREQ,    // n a fbm of t x FREQ, t the offset across it / its width, PERSIST^k
                          // (a table per noise slot, lensflare_generator.cpp)
    GLARE_STREAK_PERSIST,
    GLARE_STREAK_SEED,
    GLARE_BRIGHT_RESPONSE, // Lens Flare: brightness_response, width_response (Occlusion Scale)
    GLARE_WIDTH_RESPONSE,
    GLARE_TEX,            // Lens Flare: 1 when an image multiplies the element (or is it)
    GLARE_TEX_START,      // its first texel's index (floats), its size in texels
    GLARE_TEX_W,
    GLARE_TEX_H,
    GLARE_TEX_X,          // its centre (pixels, y up)
    GLARE_TEX_Y,
    GLARE_TEX_M00,        // a pixel's offset from the centre to texels (column, row), the
    GLARE_TEX_M01,        // texel (w / 2, h / 2) at the centre
    GLARE_TEX_M10,
    GLARE_TEX_M11,
    GLARE_REL_X,          // Lens Flare: rel_width, rel_height x Rel Heights, in the screen's
    GLARE_REL_Y,          // frame (GLARE_SCALE_*: the stretch alone)
    GLARE_DISTORT,        // Lens Flare: stretch_distort's strength (ms_glare::element_value)
    GLARE_RAY_MIN,        // RAYS: the shortest line / the longest (1: all the longest)
    GLARE_RAY_SEED,       // RAYS: the lines' random lengths and turns: seed, jitter_angles
    GLARE_RAY_JITTER,
    GLARE_START,      // the sector's start angle (and the polygon's first vertex)
    GLARE_WIDTH,      // the sector's angular width (2 pi: whole)
    GLARE_SOFTNESS,   // arc_edge_softness
    GLARE_REPEATS,    // the angular window's repeats
    GLARE_SIDES,      // polygon sides (0: a circle)
    // Lens Flare: 1 when the polygon is irregular (random_side_lengths), its vertices' angles
    // from the first (GLARE_START), increasing, then 2 pi, then more than 2 pi up to
    // GLARE_MAX_SIDES (else regular: GLARE_SIDES alone)
    GLARE_IRREGULAR,
    GLARE_VERTICES,
    GLARE_VERTICES_END = GLARE_VERTICES + GLARE_MAX_SIDES,
    GLARE_ROUNDNESS,  // the polygon's edge radius lerped towards the circle
    GLARE_FADE,       // fade_gradient
    GLARE_SIGMA,      // the radial profile's gaussian blur (Blur Gradient), 0: none
    GLARE_BOOST,      // 1 + offset_bright
    GLARE_WAVE,       // wave_brightness
    GLARE_WAVE_FREQ,
    GLARE_WAVE_WIDTH,
    GLARE_RW_MIN,     // radius waves: min, max, frequency, spikyness
    GLARE_RW_MAX,
    GLARE_RW_FREQ,
    GLARE_RW_SPIKE,
    GLARE_LINES,      // RAYS: number of lines
    GLARE_RAY_WIDTH,  // RAYS: the half width across a line
    GLARE_STOP_COUNT, // STOPS: number of stops
    GLARE_STOP_RADII, // STOPS: GLARE_MAX_STOPS radii (fractions of R)
    GLARE_STOP_COLORS = GLARE_STOP_RADII + GLARE_MAX_STOPS, // their RGB colours
    GLARE_FIELDS = GLARE_STOP_COLORS + 3 * GLARE_MAX_STOPS,
};
