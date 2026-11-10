// The list of effects. Each host adapter (ofx/plugin.c) makes a plugin of each one.

#include "common/defines.h"
#include "common/effect.h"

const MsEffect *const ms_effects[] = {
    &ms_test_effect,
    &ms_bandpass_effect,
    &ms_edgeawareblur_effect,
    &ms_distort_effect,
    &ms_mosaic_effect,
    &ms_glow_effect,
    &ms_edgeblur_effect,
    &ms_rackdefocus_effect,
    &ms_glint_effect,
    &ms_glare_effect,
};

const int ms_effect_count = ARRAY_SIZE(ms_effects);
