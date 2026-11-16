// The effects' choices per API. See profile.h.

#include "effects/profile.h"

// soft_gpu: Soft on the GPU (the M1 Pro: 123 ms at 4K, on its CPU 73).
#define MEASURED_ON_M1_PRO(soft)                                                                   \
    {.sharp_short_margin = 128,                                                                    \
     .rows_margin = 48,                                                                            \
     .softer_gpu = 1,                                                                              \
     .soft_gpu = (soft),                                                                           \
     .bandpass_staged = 1,                                                                         \
     .bandpass_prefix = 24,                                                                        \
     .blur_prefix = 24}

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

int ms_rgba_blur(MsApi api, MsImage *input, int rx, int ry, MsImage *output) {
    // The prefix version costs the same whatever the radius: for large blurs. The largest box
    // of a blur of total radius t is (t + 2) / 3.
    const MsProfile *p = ms_profile(api);
    const int t = rx > ry ? rx : ry;
    const MsKernel kernel =
        ms_kernel_built(MS_K_BLUR_PREFIX, api) && p->blur_prefix && (t + 2) / 3 >= p->blur_prefix
            ? MS_K_BLUR_PREFIX
            : MS_K_BLUR;
    return MS_KERNEL(kernel, api, MS_IMAGE("input", input), MS_INT("radius_x", rx),
                     MS_INT("radius_y", ry), MS_IMAGE("output", output));
}
