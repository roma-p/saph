// The test images of the benchmark (tools/perf/bench.c --sweep): generated, written as EXR
// files that Nuke reads too (tools/perf/nuke_sweep.py), so that both time the same pixels;
// and EXR files added by hand (real plates), read back.

#pragma once

#include <stddef.h>

// A generated image. Pixels: RGBA floats, channels next to each other, bottom row first.
typedef struct {
    const char *name; // file name without ".exr"
    const char *what; // one line for the report
    int input;        // 1: made to be a Mask or Lens input rather than a Source
    int full_float;   // written in 32-bit float (else half: half the size)
    void (*make)(float *pixels, int width, int height);
} MsTestImage;

extern const MsTestImage ms_test_images[];
extern const int ms_test_image_count;

// Writes `pixels` (width x height RGBA) to `path`: an uncompressed scanline EXR, channels
// A, B, G, R in half or 32-bit float. Returns 1 on success.
int ms_exr_write(const char *path, const float *pixels, int width, int height, int full_float);

// Reads an EXR written by ms_exr_write, or any uncompressed scanline EXR of this size with
// some of the channels R, G, B, A (half, float or uint; missing ones: 0, alpha 1). Returns 1
// on success, else 0 with the reason in err.
int ms_exr_read(const char *path, float *pixels, int width, int height, char *err,
                size_t err_size);
