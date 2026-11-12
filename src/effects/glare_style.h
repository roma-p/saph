// MS_Glare's style: its JSON text (the reference plugin's format: elements in containers)
// read into the rows of halide/glare_layout.h.

#pragma once

#include "common/json.h"
#include "halide/glare_layout.h"

// The effect's settings the rows depend on.
typedef struct {
    double size;       // Size in pixels of the render (Lens Flare: its size unit)
    double rel_height; // Rel Height
    double rotate;     // Rotate, degrees
    double rays_num_scale, rays_length, rays_thickness;
    // Lens Flare (lens != 0): the elements placed along the axis from the pivot through the
    // hotspot (pixels, y up), each at pivot + center x (hotspot - pivot).
    int lens;
    double hotspot[2], pivot[2];
    double unit;        // distances' unit (pixels): the frame's width / 4
    double frame_width, frame_height; // pixels
    double gamma;       // each element's brightness b -> b^(1 / gamma)
    double hot_gain[3], other_gain[3], rays_gain; // the families' brightness x colour
    double other_width, rays_rotate;
    // The Center and Edge Triggers: (scale - 1) x their weight here, for the brightness and
    // the widths (each element scales them by its brightness_ and width_response).
    double center_bright, center_width, edge_bright, edge_width;
} MsGlareGlobals;

// Lens Flare: MillSaph's own textures the lens asks for (an element's "image": {"generate":
// "dirt" | "glint" | "ramp" | "iris", "seed", "amount", "scale", "detail", "sides",
// "texels"}), each to generate (halide/lensflare_generator.cpp ms_lensflare_texture) into the
// texels at `start`, after the lens's own images.
#define MS_LENS_MAX_GENERATED 16
typedef struct {
    int kind; // GLARE_TEXTURE_*; -1: an image written in the element itself (its node)
    double seed, amount, scale, detail;
    int sides, width, height;
    size_t start;
    int node, channels; // (an image in the element: its node, 1 or 3 channels)
} MsLensTexture;
typedef struct {
    MsLensTexture list[MS_LENS_MAX_GENERATED];
    int count;
    size_t texels; // the texels of the images and these together (floats)
} MsLensTextures;

// The elements of style `text` into `rows` (at most `max_rows`), using `nodes` (at most
// `max_nodes`: the text's length is always enough) for the JSON tree (Lens Flare: the
// shadows' rows among them, GLARE_HIDDEN). Returns the number of
// rows, or -1 if the text is not a style.
int ms_glare_style(const char *text, MsJson *nodes, int max_nodes, const MsGlareGlobals *g,
                   float (*rows)[GLARE_FIELDS], int max_rows, MsLensTextures *textures);

// Lens Flare: the texels of the lens's images (its "images", and those written in elements:
// 8-bit pixels in base64, RGB or grey), as `nodes` holds the lens after ms_glare_style, into
// `out` (as GLARE_TEX_START counts them; RGB, grey repeated); the generated ones are left to
// the caller (`textures`' entries of kind >= 0).
void ms_lens_texels(const MsJson *nodes, const MsLensTextures *textures, float *out);
