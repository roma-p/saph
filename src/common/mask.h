// The parameters every effect with a Mask input declares. The mask value they set is
// computed by the kernels (src/halide/mask.h).

#pragma once

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
