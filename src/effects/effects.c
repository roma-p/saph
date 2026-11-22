// The list of effects (effects/list.def). Each host adapter (ofx/plugin.c) makes a plugin of each
// one.

#include <stdio.h>

#include "common/defines.h"
#include "common/effect.h"

const MsEffect *const ms_effects[] = {
    &ms_test_effect,
#define MS_EFFECT(Name, name, pipl_id, varies) &ms_##name##_effect,
#include "effects/list.def"
#undef MS_EFFECT
};

const int ms_effect_count = ARRAY_SIZE(ms_effects);

void ms_effects_release(void) {
    for (int i = 0; i < ms_effect_count; ++i)
        if (ms_effects[i]->release)
            ms_effects[i]->release();
}

int ms_out_of_memory(const MsRender *r) {
    snprintf(r->err, r->err_size, "not enough memory");
    return 0;
}
