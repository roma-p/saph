// The effects' choices per API. See profile.h.

#include "effects/profile.h"

// soft_gpu: Soft on the GPU (the M1 Pro: 123 ms at 4K, on its CPU 73).
#define MEASURED_ON_M1_PRO(soft)                                                                   \
    {.sharp_short_margin = 128,                                                                    \
     .rows_margin = 48,                                                                            \
     .softer_gpu = 1,                                                                              \
     .soft_gpu = (soft),                                                                           \
     .bandpass_staged = 1,                                                                         \
     .bandpass_prefix = 24}

static MsProfile profiles[MS_API_COUNT] = {
    [MS_API_METAL] = MEASURED_ON_M1_PRO(0),
    [MS_API_CUDA] = MEASURED_ON_M1_PRO(1),   // to measure (Soft on the GPU: to try)
    [MS_API_VULKAN] = MEASURED_ON_M1_PRO(1), // to measure
    [MS_API_D3D12] = MEASURED_ON_M1_PRO(1),  // to measure
    [MS_API_CPU] = MEASURED_ON_M1_PRO(0),    // not used
};

const MsProfile *ms_profile(MsApi api) { return &profiles[api < MS_API_COUNT ? api : MS_API_CPU]; }

void ms_profile_set(MsApi api, const MsProfile *profile) {
    if (api < MS_API_COUNT)
        profiles[api] = *profile;
}
