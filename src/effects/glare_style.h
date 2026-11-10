// MS_Glare's style: its JSON text (the reference plugin's format: elements in containers)
// read into the rows of halide/glare_layout.h.

#pragma once

#include "common/json.h"
#include "halide/glare_layout.h"

// The effect's settings the rows depend on.
typedef struct {
    double size;       // Size in pixels of the render
    double rel_height; // Rel Height
    double rotate;     // Rotate, degrees
    double rays_num_scale, rays_length, rays_thickness;
} MsGlareGlobals;

// The elements of style `text` into `rows` (at most `max_rows`), using `nodes` (at most
// `max_nodes`: the text's length is always enough) for the JSON tree. Returns the number of
// rows, or -1 if the text is not a style.
int ms_glare_style(const char *text, MsJson *nodes, int max_nodes, const MsGlareGlobals *g,
                   float (*rows)[GLARE_FIELDS], int max_rows);
