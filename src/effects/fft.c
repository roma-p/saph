// Fast Fourier transforms of images: the stages of src/halide/fft_generator.cpp in turn.

#include "effects/fft.h"

#include "common/defines.h"

int ms_fft_length(int length) {
    for (int n = length > 1 ? length : 1;; ++n) {
        int m = n;
        while (m % 2 == 0)
            m /= 2;
        while (m % 3 == 0)
            m /= 3;
        while (m % 5 == 0)
            m /= 5;
        if (m == 1)
            return n;
    }
}

// The radices of length `n` (a product of 2, 3, 5): the largest that divide it first, up to
// 8 (each stage reads and writes the whole image, and costs a product per radix and point:
// on an RTX A6000, 8 is the fastest largest radix, 16 and 32 slower, 4 too).
static int radices(int n, int out[32]) {
    static const int order[] = {8, 6, 5, 4, 3, 2};
    int count = 0;
    for (int i = 0; i < (int)ARRAY_SIZE(order); ++i)
        while (n % order[i] == 0) {
            out[count++] = order[i];
            n /= order[i];
        }
    return count;
}

// The stages along one axis.
static int transform(MsApi api, MsImage **data, MsImage **scratch, int along_y, float sign) {
    int r[32];
    const int count = radices((*data)->dim[along_y].extent, r);
    for (int i = 0, ns = 1; i < count; ns *= r[i++]) {
        const int status =
            MS_KERNEL(MS_K_FFT_STAGE, api, MS_IMAGE("in", *data), MS_INT("along_y", along_y),
                      MS_INT("radix", r[i]), MS_INT("ns", ns), MS_FLOAT("sign", sign),
                      MS_IMAGE("out", *scratch));
        if (status != 0)
            return status;
        MsImage *done = *scratch;
        *scratch = *data;
        *data = done;
    }
    return 0;
}

int ms_fft(MsApi api, MsImage **data, MsImage **scratch, int inverse) {
    const float sign = inverse ? 1.0f : -1.0f;
    int status = transform(api, data, scratch, inverse ? 1 : 0, sign);
    if (status == 0)
        status = transform(api, data, scratch, inverse ? 0 : 1, sign);
    return status;
}
