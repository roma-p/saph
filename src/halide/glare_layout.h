// MS_Glare's style as the kernels get it: one row of GLARE_FIELDS floats per element (the
// elements that draw, the containers flattened), built by effects/glare_style.c from the
// style's text and read by src/halide/glare_generator.cpp. Shared by C and C++.

#pragma once

#define GLARE_MAX_ELEMENTS 256
#define GLARE_MAX_STOPS 32 // colour stops of an element
#define GLARE_MAX_LINES 64 // ray lines of a rays element

// An element's kind (GLARE_KIND).
enum {
    GLARE_FALLOFF = 0, // one colour stop: the soft blob falloff
    GLARE_STOPS = 1,   // colour stops along the radius (spot, ring, blob, polar_flare, fancy_spot)
    GLARE_RAYS = 2,    // ray lines through the centre
};

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
    GLARE_CRISP,      // 1: the band's edges over one pixel (polar_flare), else over ~2
    GLARE_START,      // the sector's start angle (and the polygon's first vertex)
    GLARE_WIDTH,      // the sector's angular width (2 pi: whole)
    GLARE_SOFTNESS,   // arc_edge_softness
    GLARE_REPEATS,    // the angular window's repeats
    GLARE_SIDES,      // polygon sides (0: a circle)
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
