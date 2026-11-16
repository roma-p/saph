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
    &ms_lensflare_effect,
    &ms_jpegdamage_effect,
    &ms_pixelsort_effect,
    &ms_flickermatch_effect,
    &ms_flickermatchcolor_effect,
    &ms_flicker_effect,
    &ms_tvdamage_effect,
};

const int ms_effect_count = ARRAY_SIZE(ms_effects);

void ms_effects_release(void) {
    for (int i = 0; i < ms_effect_count; ++i)
        if (ms_effects[i]->release)
            ms_effects[i]->release();
}
