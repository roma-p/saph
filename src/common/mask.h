// The Mask input, shared by the effects: the luma, the mask value of each pixel,
// and the parameters every masked effect declares. On the GPU, mask.glsl.

#pragma once

#include <stddef.h>

// Luma weights: (5 R + 8 G + 3 B) / 16.
static inline float ms_luma(const float *rgba) {
    return (5.0f * rgba[0] + 8.0f * rgba[1] + 3.0f * rgba[2]) / 16.0f;
}

// One value per pixel of the Mask input (packed RGBA): its luma, or alpha with
// `alpha`, inverted with `invert`.
void ms_mask_values(const float *rgba, size_t pixels, int alpha, int invert, float *values);

// The Mask Use and Invert Mask parameters (MsParam initializers, see effect.h).
extern const char *const ms_mask_use_entries[2];
#define MS_PARAM_MASK_USE                                                                          \
    {.type = MS_PARAM_CHOICE,                                                                      \
     .name = "mask_use",                                                                           \
     .label = "Mask Use",                                                                          \
     .hint = "How the Mask input channels make a monochrome mask.",                                \
     .entries = ms_mask_use_entries,                                                               \
     .entry_count = 2}
#define MS_PARAM_INVERT_MASK(hint_text)                                                            \
    {.type = MS_PARAM_BOOL, .name = "invert_mask", .label = "Invert Mask", .hint = (hint_text)}
