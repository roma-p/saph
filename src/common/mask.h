// The parameters every effect with a Mask input declares. The mask value they set is
// computed by the kernels (src/halide/mask.h).

#pragma once

// The Mask Use and Invert Mask parameters (MsParam initializers, see effect.h).
extern const char *const ms_mask_use_entries[2];
#define MS_PARAM_MASK_USE MS_PARAM_MASK_USE_HIDDEN(0)
// (hidden: kept in scripts and presets, not shown; MsParam.hidden)
#define MS_PARAM_MASK_USE_HIDDEN(hide)                                                             \
    {.type = MS_PARAM_CHOICE,                                                                      \
     .name = "mask_use",                                                                           \
     .label = "Mask Use",                                                                          \
     .hint = "How the Mask input channels make a monochrome mask.",                                \
     .entries = ms_mask_use_entries,                                                               \
     .entry_count = 2,                                                                             \
     .hidden = (hide)}
// Blur Mask: a width, `d` by default, its slider to `dm` (an MsParam initializer, see effect.h).
#define MS_PARAM_BLUR_MASK(d, dm)                                                                  \
    {.type = MS_PARAM_WIDTH,                                                                       \
     .name = "blur_mask",                                                                          \
     .label = "Blur Mask",                                                                         \
     .hint = "Blurs the Mask input by this amount before using.",                                  \
     .def = (d),                                                                                   \
     .min = 0.0,                                                                                   \
     .max = MS_UNBOUNDED,                                                                          \
     .display_max = (dm)}
#define MS_PARAM_INVERT_MASK(hint_text)                                                            \
    {.type = MS_PARAM_BOOL, .name = "invert_mask", .label = "Invert Mask", .hint = (hint_text)}
