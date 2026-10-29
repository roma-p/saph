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
// no padding); `outside` (all four channels) where `img` has no pixels. `buffer` must
// hold width x height x 4 floats of `area`.
void ms_pack_image(const MsImage* img, OfxRectI area, float outside, float* buffer);

// Copies the part of a packed buffer covering `area` that falls in `window` into `img`.
void ms_unpack_image(const float* buffer, OfxRectI area, MsImage* img, OfxRectI window);

// One input of an effect. Each effect lists its inputs in a table, used both to
// declare them to the host and to render.
typedef struct {
    const char* clip;   // identifier of the input for OFX and the host (saved in scripts);
                        // the main input must use kOfxImageEffectSimpleSourceClipName ("Source")
    const char* label;  // name shown to the user on the node
    const char* name;   // name the shaders read it by (`uniform sampler2D <name>`)
    float outside;      // value used where the clip has no pixels (e.g. not connected)
    int optional;       // the effect works without it
    int mask;           // hosts show it as a mask input
} MsClip;

#define MS_MAX_CLIPS 8

// Declares the inputs and the Output to the host (DescribeInContext action). In the
// "filter" context only the Source can exist; the other inputs then read `outside`.
// An effect with inputs besides Source must support only the "general" context in
// its Describe action: hosts like Nuke pick "filter" when offered and show one input.
OfxStatus ms_define_clips(OfxImageEffectHandle effect, OfxPropertySetHandle in_args, const MsClip* clips,
                          int clip_count);

// Render the effect on the GPU.
// Gets every input image, packs it, runs the shaders on the GPU, then unpacks the
// result into the host's Output. The shaders read each input by its `name`, and
// earlier results as `pass1`, `pass2`...
OfxStatus ms_render(OfxImageEffectHandle effect, OfxPropertySetHandle in_args, const MsClip* clips,
                    int clip_count, const char* const* shaders, int pass_count, const MsShaderValue* values,
                    int value_count);
