// 16-bit floats. See half.h.

#include <string.h>

#include "common/half.h"

uint16_t ms_half_from_float(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    const uint32_t sign = (u >> 16) & 0x8000u;
    u &= 0x7fffffffu;
    if (u >= 0x47800000u) // 65536 and above, infinities, NaN
        return (uint16_t)(sign | (u > 0x7f800000u ? 0x7e00u : 0x7c00u));
    if (u < 0x38800000u) { // below the smallest normal half: denormal halves
        // Adding 0.5 puts the half's denormal bits at the bottom of the float, rounded.
        float a;
        memcpy(&a, &u, sizeof a);
        a += 0.5f;
        memcpy(&u, &a, sizeof u);
        return (uint16_t)(sign | (u - 0x3f000000u));
    }
    const uint32_t odd = (u >> 13) & 1u;
    u += 0xc8000fffu + odd; // exponent from 127 to 15 based, rounded to nearest even
    return (uint16_t)(sign | (u >> 13));
}

float ms_float_from_half(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1fu,
                   mantissa = h & 0x3ffu;
    if (exponent == 0) { // zero and denormals: mantissa x 2^-24
        const float f = (float)mantissa * 5.9604644775390625e-8f;
        return sign ? -f : f;
    }
    const uint32_t u = sign | (exponent == 31 ? 0x7f800000u | (mantissa << 13)
                                              : ((exponent + 112u) << 23) | (mantissa << 13));
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}
