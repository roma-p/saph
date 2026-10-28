// ofx images helpers.

#pragma once

#include "gpu.h"
#include "millsaph.h"

// data struct wrapping ofx image handle.
// image are considered always RGBA 32 bit flaot.
// (up to the host (dcc) to do the conversion)
typedef struct {
    OfxPropertySetHandle handle;    // handle to the ofx image data structure
    char* data;                     // pointers to first pixels. typed to char* for p arithmeric (to use with row_bytes)
    int row_bytes;                  // row_bytes: row dimension (can be bigger than width, can be neg).
    OfxRectI bounds;                // rectangle describing the actual image.
} MsImage;

// Gets a clip's image at `time`. Returns 0 if there is none (e.g. the input is not
// connected).
// !! need to call ms_release_image
int ms_fetch_image(OfxImageEffectHandle effect, const char* clip, OfxTime time, MsImage* img);

// Gives the image back to the host.
void ms_release_image(MsImage* img);

// RGBA pixel at (x, y), or NULL outside the image.
float* ms_pixel(const MsImage* img, int x, int y);

// Fills `buffer` with the pixels of `img` covering `area`, packed (bottom row first,
// no padding); black where `img` has no pixels. `buffer` must hold
// width x height x 4 floats of `area`.
void ms_pack_image(const MsImage* img, OfxRectI area, float* buffer);

// Copies the part of a packed buffer covering `area` that falls in `window` into `img`.
void ms_unpack_image(const float* buffer, OfxRectI area, MsImage* img, OfxRectI window);

// Render the effect on the GPU.
// get host image, pack it, send it to the gpu and run shaders, then unpack it and return it to host.
OfxStatus ms_render_shader(OfxImageEffectHandle effect, OfxPropertySetHandle in_args, const char* shader,
                           const MsUniform* uniforms, int uniform_count);
