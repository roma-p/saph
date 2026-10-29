// ofx images helpers.

#pragma once

#include "gpu/gpu.h"
#include "ofx/host.h"

#define MS_MAX_CLIPS 8

// Declares the inputs and the Output to the host (DescribeInContext action). In the
// "filter" context only the Source can exist; the other inputs then read `outside`.
// An effect with inputs besides Source must support only the "general" context in
// its Describe action: hosts like Nuke pick "filter" when offered and show one input.
OfxStatus ms_define_clips(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                          const MsClip *clips, int clip_count);

// A processing step: `inputs` are the packed input buffers (in the order of the clip
// table), `output` is the packed result to fill (width x height RGBA floats, bottom row
// first). Returns 1 on success, or 0 with the reason in err.
typedef int (*MsProcess)(const MsInput *inputs, int input_count, int width, int height,
                         float *output, char *err, size_t err_size, void *user);

// The whole render around one processing step: gets every input image and packs it,
// runs `process` on the packed buffers, then unpacks its result into the host's
// Output (or shows the error in the host).
OfxStatus ms_render_with(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                         const MsClip *clips, int clip_count, MsProcess process, void *user);
