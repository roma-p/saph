// ofx images helpers.

#pragma once

#include "common/effect.h"
#include "engine/engine.h"
#include "ofx/host.h"

// Declares the inputs and the Output to the host (DescribeInContext action). In the
// "filter" context only the Source can exist; the other inputs then read `outside`.
// An effect with inputs besides Source must support only the "general" context in
// its Describe action: hosts like Nuke pick "filter" when offered and show one input.
OfxStatus ms_define_clips(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                          const MsClip *clips, int clip_count);

// The whole render of `e`: gets every input image, renders `r` (given with its values,
// connected inputs and frame size; the rest filled here: images, size, depth) into the host's
// Output, or shows the error in the host. Images are read and written in the host's memory
// where possible. The first renders of a session are logged: where the time went, the
// settings and the API.
OfxStatus ms_render_with(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                         const MsEffect *e, MsRender *r);
