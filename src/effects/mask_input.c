// The effects' mask value. See mask_input.h.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "effects/mask_input.h"

int ms_mask_init(MsMask *m, const MsRender *r, int clip, int source_clip, MsImage *source,
                 double mask_use, double invert_mask, double blur_mask) {
    const int w = r->width, h = r->height;
    *m = (MsMask){.one = 1.0f,
                  .has_mask = r->connected[clip],
                  .alpha = (int)mask_use,
                  .invert = (int)invert_mask,
                  .blur = (int)floor(blur_mask + 0.5)};
    m->pixels = m->has_mask ? malloc((size_t)w * h * sizeof(float)) : NULL;
    if (m->has_mask && !m->pixels) {
        snprintf(r->err, r->err_size, "not enough memory");
        return 0;
    }
    ms_image_rgba(&m->mask, r->inputs[clip].pixels, r->inputs[clip].row, r->half, w, h);
    if (m->has_mask)
        ms_image(&m->value, m->pixels, w, w, h, 1);
    else
        ms_image_constant(&m->value, &m->one, w, h);
    m->input = !m->has_mask ? &m->value
               : ms_same_input(&r->inputs[clip], &r->inputs[source_clip]) ? source
                                                                           : &m->mask;
    return 1;
}

int ms_mask_run(MsMask *m, MsApi api) {
    if (!m->has_mask)
        return 0;
    return MS_KERNEL(MS_K_MASK, api, MS_IMAGE("mask", m->input), MS_INT("mask_alpha", m->alpha),
                     MS_INT("invert_mask", m->invert), MS_INT("blur_mask", m->blur),
                     MS_IMAGE("mask_value", &m->value));
}

void ms_mask_free(MsMask *m) {
    free(m->pixels);
    m->pixels = NULL;
}
