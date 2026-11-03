// Limits of kernel versions, which the effects check before choosing them: shared by the
// generators (C++) and the effects (C).

#pragma once

// BandPass, staged GPU version (blur.h): box radii it holds in on-chip memory.
#define MS_STAGED_RADIUS_X 200
#define MS_STAGED_RADIUS_Y 100
