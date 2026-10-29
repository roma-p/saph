// Edge-aware filtering with the Domain Transform (Gastal & Oliveira, SIGGRAPH 2011),
// with the settings MS_EdgeAwareBlur uses. CPU, all cores.
//
// Images are packed RGBA floats, width x height, bottom row first. Every buffer is
// allocated by the caller.

#pragma once

typedef enum {
    MS_DT_SHARP,  // normalised convolution: box in the transformed domain
    MS_DT_SOFT,   // recursive filter: exponential in the transformed domain
    MS_DT_SOFTER, // interpolated convolution
} MsDtType;

typedef struct {
    MsDtType type;
    int passes;            // Box 1, Triangle 2, Gauss 3, Smooth 5
    double blur_x, blur_y; // width in pixels per direction (0: no blur that way)
} MsDtSettings;

// Distances between neighbours in the transformed domain: dx[i] links pixel i to the
// pixel on its left, dy[i] to the pixel below (first column / row unused).
//   distance = 1 / max(m, mask_floor) + scale_(x|y) x contrast
// m = the smaller `mask` value of the two pixels (1 without a mask, `mask` may be NULL):
// a lower mask stretches distances, so it blurs less, and 0 does not blur.
// contrast = sum of |RGB difference|, or with `use_alpha` 0.75 x (sum of |RGBA difference|).
void ms_dt_distances(const float *edge, int width, int height, double scale_x, double scale_y,
                     int use_alpha, const float *mask, double mask_floor_x, double mask_floor_y,
                     float *dx, float *dy);

// Filters `image` in place. Returns 0 if the work could not be split over threads
// (the result is still computed).
int ms_dt_filter(float *image, int width, int height, const float *dx, const float *dy,
                 const MsDtSettings *settings);
