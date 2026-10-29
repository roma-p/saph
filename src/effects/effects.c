// The list of effects. Each host adapter (ofx/plugin.c) makes a plugin of each one.

#include "common/defines.h"
#include "common/effect.h"

const MsEffect *const ms_effects[] = {
    &ms_test_effect,
    &ms_bandpass_effect,
    &ms_edgeawareblur_effect,
    &ms_distort_effect,
};

const int ms_effect_count = ARRAY_SIZE(ms_effects);
