// A clock for timing. See clock.h.

// clock_gettime: hidden by glibc in strict C11 without it.
#define _POSIX_C_SOURCE 200809L

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#include "common/clock.h"

double ms_clock(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart * 1e3 / (double)frequency.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
#endif
}
