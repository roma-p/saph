// Choices the effects make that depend on the GPU, per API: which version of a kernel is
// faster, or whether the CPU is. Measured on an M1 Pro (Metal); the other APIs use the same
// values until measured (tools/perf/bench.c varies them). The effects read them in their run
// function, once the API is known.

#pragma once

#include "engine/engine.h"

typedef struct {
    int sharp_short_margin; // EdgeAwareBlur Sharp: tile-64 version up to this margin
    int rows_margin;        // EdgeAwareBlur Sharp / Softer: "rows" versions from this margin on
    int softer_gpu;         // EdgeAwareBlur Softer: on the GPU (1) or the CPU (0)
    int soft_gpu;           // EdgeAwareBlur Soft: on the GPU (1) or the CPU (0)
    int bandpass_staged;    // BandPass: staged version where it fits (1), never (0)
    int bandpass_prefix;    // BandPass: prefix version from this box radius on (0: never)
} MsProfile;

const MsProfile *ms_profile(MsApi api);

// Benchmarks: replaces the profile of `api`.
void ms_profile_set(MsApi api, const MsProfile *profile);
