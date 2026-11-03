// Page-locking the host's images for the CUDA copies. See internal.h.
//
// Copies between the GPU and page-locked memory run at the bus's speed; from ordinary
// ("pageable") memory the driver goes through its own buffers, about 2.5 times slower
// (millsaph_bench --transfer on an RTX A6000, HD: 13.9 ms to the GPU and back, 6.4 with
// the memory locked for the copies, locking and unlocking included). The CUDA driver is
// called directly (loaded at run time, as Halide's runtime does), in the context of Halide's
// GPU memory.

#include <stdio.h>

#include "common/log.h"
#include "common/mutex.h"
#include "engine/internal.h"

static int enabled = 1; // ms_pin_host

void ms_pin_host(int on) { enabled = on; }

#ifndef MILLSAPH_API_CUDA
void *ms_pin_context(const halide_buffer_t *gpu) {
    (void)gpu;
    return NULL;
}
int ms_pin(MsPinned *p, const halide_buffer_t *b, void *context) {
    (void)p;
    (void)b;
    (void)context;
    return 0;
}
void ms_unpin(MsPinned *p, void *context) {
    (void)p;
    (void)context;
}
#else
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define open_driver() ((void *)LoadLibraryA("nvcuda.dll"))
#define find(lib, name) ((void *)(uintptr_t)GetProcAddress((HMODULE)(lib), (name)))
#define CALL __stdcall
#else
#include <dlfcn.h>
#define open_driver() dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL)
#define find(lib, name) dlsym((lib), (name))
#define CALL
#endif

uintptr_t halide_cuda_get_device_ptr(void *user_context, struct halide_buffer_t *buf);

#define CU_POINTER_ATTRIBUTE_CONTEXT 1

// The memory locked now, shared by the renders using it (two renders can read the same image
// of the host at once): locked by the first, released by the last.
#define LOCKED 64
typedef struct {
    void *start;
    size_t bytes;
    int users;
} Locked;
static Locked locked[LOCKED];
static int locked_count;
static MsMutex locked_lock = MS_MUTEX_INIT;

static int(CALL *pointer_attribute)(void *, int, unsigned long long);
static int(CALL *push_context)(void *);
static int(CALL *pop_context)(void **);
static int(CALL *host_register)(void *, size_t, unsigned);
static int(CALL *host_unregister)(void *);

// The driver's functions, loaded once; 0 when missing.
static int load(void) {
    static MsMutex lock = MS_MUTEX_INIT;
    static int tried, loaded;
    ms_mutex_lock(&lock);
    if (!tried) {
        tried = 1;
        void *driver = open_driver(); // kept loaded: Halide's runtime uses it too
        if (driver) {
            *(void **)&pointer_attribute = find(driver, "cuPointerGetAttribute");
            *(void **)&push_context = find(driver, "cuCtxPushCurrent_v2");
            *(void **)&pop_context = find(driver, "cuCtxPopCurrent_v2");
            *(void **)&host_register = find(driver, "cuMemHostRegister_v2");
            *(void **)&host_unregister = find(driver, "cuMemHostUnregister");
        }
        loaded = pointer_attribute && push_context && pop_context && host_register &&
                 host_unregister;
        if (!loaded)
            ms_log("CUDA: cannot page-lock the images (driver functions missing): slower copies");
    }
    const int ok = loaded;
    ms_mutex_unlock(&lock);
    return ok;
}

void *ms_pin_context(const halide_buffer_t *gpu) {
    void *context = NULL;
    if (!enabled || !gpu->device || !load())
        return NULL;
    const uintptr_t pointer = halide_cuda_get_device_ptr(NULL, (halide_buffer_t *)gpu);
    if (!pointer || pointer_attribute(&context, CU_POINTER_ATTRIBUTE_CONTEXT, pointer) != 0)
        return NULL;
    return context;
}

int ms_pin(MsPinned *p, const halide_buffer_t *b, void *context) {
    p->start = NULL;
    if (!context || !b->host)
        return 0;
    // The bytes the image spans: its strides can be negative (rows top first), or 0.
    ptrdiff_t low = 0, high = 0;
    for (int d = 0; d < b->dimensions; ++d) {
        const ptrdiff_t span = (ptrdiff_t)(b->dim[d].extent - 1) * b->dim[d].stride;
        if (span < 0)
            low += span;
        else
            high += span;
    }
    const size_t element = (size_t)((b->type.bits + 7) / 8);
    if (high - low + 1 < 1024) // a constant or a tiny image: not worth it
        return 0;
    void *start = b->host + low * (ptrdiff_t)element;
    const size_t bytes = (size_t)(high - low + 1) * element;
    ms_mutex_lock(&locked_lock);
    int found = -1;
    for (int i = 0; i < locked_count && found < 0; ++i)
        if (locked[i].start == start && locked[i].bytes == bytes)
            found = i;
    if (found >= 0) { // locked already by another render
        ++locked[found].users;
        p->start = start;
    } else if (locked_count < LOCKED) {
        // Fails for memory locked otherwise (another image on the same pages, or the
        // host's own): then copied as it is.
        void *popped;
        int status = push_context(context);
        if (status == 0) {
            status = host_register(start, bytes, 0);
            pop_context(&popped);
        }
        static int reported;
        if (status != 0 && !reported) {
            reported = 1;
            ms_log("CUDA: page-locking an image failed (error %d): copied as it is", status);
        }
        if (status == 0) {
            locked[locked_count++] = (Locked){start, bytes, 1};
            p->start = start;
        }
    }
    ms_mutex_unlock(&locked_lock);
    return p->start != NULL;
}

void ms_unpin(MsPinned *p, void *context) {
    if (!p->start)
        return;
    ms_mutex_lock(&locked_lock);
    for (int i = 0; i < locked_count; ++i) {
        if (locked[i].start != p->start || --locked[i].users > 0)
            continue;
        void *popped;
        if (push_context(context) == 0) {
            host_unregister(p->start);
            pop_context(&popped);
        }
        locked[i] = locked[--locked_count];
        break;
    }
    ms_mutex_unlock(&locked_lock);
    p->start = NULL;
}
#endif
