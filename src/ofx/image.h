// ofx images helpers.

#pragma once

#include "common/effect.h"
#include "ofx/host.h"

#define MS_MAX_CLIPS 8

// Declares the inputs and the Output to the host (DescribeInContext action). In the
// "filter" context only the Source can exist; the other inputs then read `outside`.
// An effect with inputs besides Source must support only the "general" context in
// its Describe action: hosts like Nuke pick "filter" when offered and show one input.
OfxStatus ms_define_clips(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                          const MsClip *clips, int clip_count);

// A processing step: `inputs` are the input images (in the order of the clip table; no
// pixels for those not connected), `output` the result to fill (width x height RGBA, bottom
// row first, `output_row` values from one row to the next); all in floats, or with `half` in
// 16-bit floats. Returns 1 on success, or 0 with the reason in err.
typedef int (*MsProcess)(const MsInput *inputs, int input_count, int half, int width, int height,
                         void *output, ptrdiff_t output_row, char *err, size_t err_size,
                         void *user);

// The whole render around one processing step: gets every input image (`connected`: one per
// input), runs `process` on them, and writes its result into the host's Output (or shows the
// error in the host). Images are read and written in the host's memory where possible.
OfxStatus ms_render_with(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                         const MsClip *clips, const int *connected, int clip_count,
                         MsProcess process, void *user);
