// GPU memory kept between renders. See internal.h.

#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

#include "common/mutex.h"
#include "engine/internal.h"

// The GPU memory of the images of a render, kept for the next renders instead of freed:
// a new allocation is slow to write the first time (and Halide only keeps allocations for
// CUDA). At most SLOTS allocations and limit() bytes in all, the oldest freed first; small
// ones are not kept.
#define SLOTS 16
#define SMALLEST ((size_t)1 << 20u)

typedef struct {
    uint64_t device;
    const struct halide_device_interface_t *interface;
    size_t bytes;
    uint64_t last_use;
} Parked;

static Parked parked[SLOTS];
static int parked_count;
static uint64_t clock_now;
static MsMutex lock = MS_MUTEX_INIT;

// The memory of the machine in bytes (0 if unknown).
static uint64_t machine_memory(void) {
#ifdef _WIN32
    MEMORYSTATUSEX status = {.dwLength = sizeof status};
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#elif defined(__APPLE__)
    uint64_t bytes = 0;
    size_t size = sizeof bytes;
    return sysctlbyname("hw.memsize", &bytes, &size, NULL, 0) == 0 ? bytes : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0;
#endif
}

// GPU memory kept at most: 1/16 of the machine's memory (the host needs the rest; on a Mac
// the GPU shares it), between 256 MB and 2 GB.
static size_t limit(void) {
    static size_t bytes;
    if (!bytes) {
        const uint64_t quarter_gb = (uint64_t)1 << 28u, two_gb = (uint64_t)1 << 31u;
        uint64_t b = machine_memory() / 16;
        b = b < quarter_gb ? quarter_gb : b > two_gb ? two_gb : b;
        bytes = (size_t)b;
    }
    return bytes;
}

// Bytes the buffer's pixels span.
static size_t buffer_bytes(const halide_buffer_t *b) {
    size_t span = 1;
    for (int d = 0; d < b->dimensions; ++d) {
        const int64_t stride = b->dim[d].stride < 0 ? -(int64_t)b->dim[d].stride : b->dim[d].stride;
        span += (size_t)(b->dim[d].extent - 1) * (size_t)stride;
    }
    return span * ((b->type.bits + 7) / 8);
}

static const uint64_t device_dirty = halide_buffer_flag_device_dirty;

// Frees GPU memory that is not in any buffer (caller holds the lock).
static void free_device(uint64_t device, const struct halide_device_interface_t *interface,
                        size_t bytes) {
    halide_dimension_t dim = {0, (int32_t)bytes, 1, 0};
    halide_buffer_t b = {0};
    b.device = device;
    b.device_interface = interface;
    b.type.code = halide_type_uint;
    b.type.bits = 8;
#if HALIDE_VERSION_MAJOR < 22
    b.type.lanes = 1;
#endif
    b.dimensions = 1;
    b.dim = &dim;
    halide_device_free(NULL, &b);
}

// Kept memory fits when it is at least the buffer's size and at most 1.5 times it.
void ms_pool_take(halide_buffer_t *b, const struct halide_device_interface_t *interface) {
    if (b->device || !interface)
        return;
    const size_t bytes = buffer_bytes(b);
    ms_mutex_lock(&lock);
    int best = -1;
    for (int i = 0; i < parked_count; ++i)
        if (parked[i].interface == interface && parked[i].bytes >= bytes &&
            parked[i].bytes <= bytes + bytes / 2 &&
            (best < 0 || parked[i].bytes < parked[best].bytes))
            best = i;
    if (best >= 0) {
        b->device = parked[best].device;
        b->device_interface = parked[best].interface;
        b->flags &= ~device_dirty;
        parked[best] = parked[--parked_count];
    }
    ms_mutex_unlock(&lock);
}

void ms_pool_park(halide_buffer_t *b, int keep) {
    if (!b->device)
        return;
    const size_t bytes = buffer_bytes(b);
    if (!keep || bytes < SMALLEST || bytes > limit()) {
        halide_device_free(NULL, b);
        return;
    }
    ms_mutex_lock(&lock);
    size_t total = bytes;
    for (int i = 0; i < parked_count; ++i)
        total += parked[i].bytes;
    // Room for it: free the oldest kept ones.
    while (parked_count > 0 && (parked_count == SLOTS || total > limit())) {
        int oldest = 0;
        for (int i = 1; i < parked_count; ++i)
            if (parked[i].last_use < parked[oldest].last_use)
                oldest = i;
        total -= parked[oldest].bytes;
        free_device(parked[oldest].device, parked[oldest].interface, parked[oldest].bytes);
        parked[oldest] = parked[--parked_count];
    }
    parked[parked_count++] = (Parked){b->device, b->device_interface, bytes, ++clock_now};
    ms_mutex_unlock(&lock);
    b->device = 0;
    b->device_interface = NULL;
    b->flags &= ~device_dirty;
}

void ms_pool_free_all(void) {
    ms_mutex_lock(&lock);
    for (int i = 0; i < parked_count; ++i)
        free_device(parked[i].device, parked[i].interface, parked[i].bytes);
    parked_count = 0;
    ms_mutex_unlock(&lock);
}
