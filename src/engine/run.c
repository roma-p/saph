// Running an effect's kernels on the first API that works. See engine.h.

// clock_gettime: hidden by glibc in strict C11 without it.
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#include "common/log.h"
#include "common/mutex.h"
#include "engine/internal.h"

// The device interfaces of Halide's runtime (declared here: some of its HalideRuntime<API>.h
// headers are C++ only).
const struct halide_device_interface_t *halide_metal_device_interface(void);
const struct halide_device_interface_t *halide_cuda_device_interface(void);
const struct halide_device_interface_t *halide_vulkan_device_interface(void);
const struct halide_device_interface_t *halide_d3d12compute_device_interface(void);

// The APIs built for this platform, in the order they are tried (CMakeLists.txt defines
// MILLSAPH_API_<API> for the GPU ones).
static const MsApi apis[] = {
#ifdef MILLSAPH_API_METAL
    MS_API_METAL,
#endif
#ifdef MILLSAPH_API_CUDA
    MS_API_CUDA,
#endif
#ifdef MILLSAPH_API_VULKAN
    MS_API_VULKAN,
#endif
#ifdef MILLSAPH_API_D3D12
    MS_API_D3D12,
#endif
    MS_API_CPU,
};

static const char *const names[MS_API_COUNT] = {"Metal", "CUDA", "Vulkan", "Direct3D 12", "CPU"};

// APIs that failed once in this session: not tried again. APIs already reported as used.
static int failed[MS_API_COUNT];
static int used[MS_API_COUNT];
static MsApi only = MS_API_COUNT; // ms_api_only

#define MAX_INPUTS 4 // images an effect uploads (ms_run's inputs)
static MsMutex lock = MS_MUTEX_INIT;

static double now_ms(void) {
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

const struct halide_device_interface_t *ms_device_interface(MsApi api) {
    switch (api) {
#ifdef MILLSAPH_API_METAL
    case MS_API_METAL:
        return halide_metal_device_interface();
#endif
#ifdef MILLSAPH_API_CUDA
    case MS_API_CUDA:
        return halide_cuda_device_interface();
#endif
#ifdef MILLSAPH_API_VULKAN
    case MS_API_VULKAN:
        return halide_vulkan_device_interface();
#endif
#ifdef MILLSAPH_API_D3D12
    case MS_API_D3D12:
        return halide_d3d12compute_device_interface();
#endif
    default:
        return NULL;
    }
}

const char *ms_api_name(MsApi api) { return api < MS_API_COUNT ? names[api] : "?"; }

int ms_api_built(MsApi api) {
    for (size_t i = 0; i < sizeof apis / sizeof apis[0]; ++i)
        if (apis[i] == api)
            return 1;
    return 0;
}

void ms_api_only(MsApi api) { only = api; }

void ms_engine_shutdown(void) { ms_pool_free_all(); }

int ms_image_device(MsImage *image, MsApi api) {
    ms_catch_errors();
    const struct halide_device_interface_t *interface = ms_device_interface(api);
    if (!interface)
        return 0;
    ms_pool_take(&image->b, interface);
    if (image->b.device || image->b.host)
        return 0; // with host memory, Halide allocates GPU memory itself when needed
    return halide_device_malloc(NULL, &image->b, interface);
}

// Errors that say the API does not work here (no driver, no device, a failed launch...): it
// is not tried again. Others (memory full, a kernel's bounds) can pass with the next render.
static int api_broken(int status) {
    switch (status) {
    case halide_error_code_generic_error: // what the GPU runtimes return when they cannot start
    case halide_error_code_copy_to_host_failed:
    case halide_error_code_copy_to_device_failed:
    case halide_error_code_device_sync_failed:
    case halide_error_code_device_free_failed:
    case halide_error_code_no_device_interface:
    case halide_error_code_symbol_not_found:
    case halide_error_code_device_run_failed:
    case halide_error_code_gpu_device_error:
    case halide_error_code_device_interface_no_device:
    case halide_error_code_device_buffer_copy_failed:
    case halide_error_code_incompatible_device_interface:
        return 1;
    default:
        return 0;
    }
}

// Gives an image of a GPU attempt kept GPU memory where it fits, and (an image without
// host memory and of a known size) new memory otherwise.
static int prepare(MsImage *image, MsApi api) {
    if (image->b.host || image->b.dim[0].extent > 0)
        return ms_image_device(image, api);
    return 0;
}

// Uploads a constant image (strides 0) as its one value: Halide would copy that value to the
// GPU once per pixel (CUDA: 12 s at HD).
static int upload_constant(MsImage *image, MsApi api) {
    const int32_t width = image->dim[0].extent, height = image->dim[1].extent;
    image->dim[0].extent = image->dim[1].extent = 1;
    const int status = halide_copy_to_device(NULL, &image->b, ms_device_interface(api));
    image->dim[0].extent = width;
    image->dim[1].extent = height;
    return status;
}

// The host's images in 16-bit floats (ms_image_rgba): converted on the API of the attempt,
// the inputs to float before the kernels, the output from float after them. `image`'s float
// pixels: GPU memory, or on the CPU `floats` (given here, freed by the caller); `view`: the
// 16-bit pixels themselves, page-locked for CUDA's copies with `context`.
static int to_float(MsImage *image, MsApi api, MsImage *view, MsPinned *pin, int *pinned,
                    void *context) {
    const int width = image->dim[0].extent, height = image->dim[1].extent;
    ms_image_half(view, image->half, image->half_row, width, height);
    if (api != MS_API_CPU) {
        view->b.flags |= (uint64_t)halide_buffer_flag_host_dirty;
        ms_pool_take(&view->b, ms_device_interface(api));
        *pinned += ms_pin(pin, &view->b, context);
    }
    return MS_KERNEL(MS_K_TO_FLOAT, api, MS_IMAGE("input", view), MS_IMAGE("output", image));
}

static int to_half(MsImage *image, MsApi api, MsImage *view, MsPinned *pin, int *pinned,
                   void *context) {
    const int width = image->dim[0].extent, height = image->dim[1].extent;
    ms_image_half(view, image->half, image->half_row, width, height);
    if (api != MS_API_CPU) {
        ms_pool_take(&view->b, ms_device_interface(api));
        *pinned += ms_pin(pin, &view->b, context);
    }
    int status = MS_KERNEL(MS_K_TO_HALF, api, MS_IMAGE("input", image), MS_IMAGE("output", view));
    if (status == 0 && api != MS_API_CPU)
        status = halide_copy_to_host(NULL, &view->b);
    return status;
}

int ms_run(MsRun run, void *user, MsImage *const *inputs, int input_count, MsImage *const *between,
           int between_count, MsImage *output, MsRunInfo *info, char *err, size_t err_size) {
    ms_catch_errors();
    MsRunInfo none = {MS_API_COUNT, 0, 0, 0};
    if (!info)
        info = &none;
    *info = none;
    // Keep Halide's GPU buffers between frames instead of allocating them each time.
    halide_reuse_device_allocations(NULL, 1);
    if (input_count > MAX_INPUTS) {
        snprintf(err, err_size, "internal error: %d inputs", input_count);
        return 0;
    }
    int status = -1;
    for (size_t i = 0; i < sizeof apis / sizeof apis[0]; ++i) {
        const MsApi api = apis[i];
        if (only != MS_API_COUNT && api != only)
            continue;
        ms_mutex_lock(&lock);
        const int skip = failed[api];
        ms_mutex_unlock(&lock);
        if (skip)
            continue;
        // The images in half (index input_count: the output): on the CPU, float pixels of
        // their own in host memory; on a GPU, in GPU memory (prepare below).
        MsImage *const io[MAX_INPUTS + 1] = {
            input_count > 0 ? inputs[0] : output, input_count > 1 ? inputs[1] : output,
            input_count > 2 ? inputs[2] : output, input_count > 3 ? inputs[3] : output, output};
        float *floats[MAX_INPUTS + 1] = {NULL};
        status = 0;
        for (int k = 0; k <= MAX_INPUTS && api == MS_API_CPU; ++k) {
            MsImage *image = k < input_count || k == MAX_INPUTS ? io[k] : NULL;
            if (!image || !image->half || status != 0)
                continue;
            floats[k] = malloc((size_t)image->dim[0].extent * image->dim[1].extent * 4 *
                               sizeof(float));
            image->b.host = (uint8_t *)floats[k];
            if (!floats[k])
                status = halide_error_code_out_of_memory;
        }
        // GPU: the images get GPU memory kept from earlier renders where it fits; the
        // inputs are marked as new in host memory (Halide uploads them).
        for (int k = 0; k < input_count && status == 0; ++k) {
            status = prepare(inputs[k], api);
            if (api != MS_API_CPU && !inputs[k]->half) {
                inputs[k]->b.flags |= (uint64_t)halide_buffer_flag_host_dirty;
                if (status == 0 && inputs[k]->dim[0].stride == 0)
                    status = upload_constant(inputs[k], api);
            }
        }
        for (int k = 0; k < between_count && status == 0; ++k)
            status = prepare(between[k], api);
        if (status == 0)
            status = prepare(output, api);
        // CUDA: the host's images page-locked for the copies (pin.c), in the context of the
        // output's GPU memory (given here rather than when Halide first needs it).
        MsPinned pinned[MAX_INPUTS * 2 + 2];
        int pinned_count = 0;
        void *context = NULL;
        if (status == 0 && api == MS_API_CUDA) {
            if (!output->b.device)
                status = halide_device_malloc(NULL, &output->b, ms_device_interface(api));
            context = status == 0 ? ms_pin_context(&output->b) : NULL;
            for (int k = 0; k < input_count && context; ++k)
                pinned_count += ms_pin(&pinned[pinned_count], &inputs[k]->b, context);
            if (context)
                pinned_count += ms_pin(&pinned[pinned_count], &output->b, context);
        }
        // Timed apart: the kernels with the uploads of the inputs (Halide uploads them when a
        // kernel needs them; half ones: converted first), then the download of the result.
        MsImage views[MAX_INPUTS + 1];
        int viewed[MAX_INPUTS + 1] = {0};
        const double start = now_ms();
        for (int k = 0; k < input_count && status == 0; ++k)
            if (inputs[k]->half) {
                viewed[k] = 1;
                status = to_float(inputs[k], api, &views[k], &pinned[pinned_count], &pinned_count,
                                  context);
            }
        if (status == 0)
            status = run(api, user);
        if (status == 0 && api != MS_API_CPU)
            status = halide_device_sync(NULL, &output->b);
        const double ran = now_ms();
        if (status == 0 && output->half) {
            viewed[MAX_INPUTS] = 1;
            status = to_half(output, api, &views[MAX_INPUTS], &pinned[pinned_count],
                             &pinned_count, context);
        } else if (status == 0) {
            status = halide_copy_to_host(NULL, &output->b);
        }
        const double done = now_ms();
        for (int k = 0; k < pinned_count; ++k)
            ms_unpin(&pinned[k], context);
        for (int k = 0; k <= MAX_INPUTS; ++k) {
            if (viewed[k])
                ms_pool_park(&views[k].b, status == 0 || status == MS_RUN_DECLINE);
            if (floats[k]) {
                io[k]->b.host = NULL;
                free(floats[k]);
            }
        }
        // The GPU memory goes back to the kept allocations (all freed after a failure).
        for (int k = 0; k < input_count; ++k)
            ms_pool_park(&inputs[k]->b, status == 0 || status == MS_RUN_DECLINE);
        for (int k = 0; k < between_count; ++k)
            ms_pool_park(&between[k]->b, status == 0 || status == MS_RUN_DECLINE);
        ms_pool_park(&output->b, status == 0 || status == MS_RUN_DECLINE);
        if (status == 0) {
            ms_mutex_lock(&lock);
            const int first = !used[api];
            used[api] = 1;
            ms_mutex_unlock(&lock);
            *info = (MsRunInfo){api, ran - start, done - ran, 0};
            if (first) // once per API and session: which one the effects really run on
                ms_log("running on %s", names[api]);
            return 1;
        }
        if (status == MS_RUN_DECLINE)
            continue;
        if (api != MS_API_CPU && api_broken(status)) {
            ms_log("%s not available (Halide error %d), trying the next", names[api], status);
            ms_mutex_lock(&lock);
            failed[api] = 1;
            ms_mutex_unlock(&lock);
        } else if (api != MS_API_CPU) {
            ms_log("%s failed this render (Halide error %d), trying the next", names[api], status);
        }
    }
    if (status == MS_KERNEL_BAD_ARGS || status == MS_KERNEL_NOT_BUILT)
        snprintf(err, err_size, "internal error %d (details in the terminal)", status);
    else if (status == MS_RUN_DECLINE)
        snprintf(err, err_size, "no API to run on");
    else
        snprintf(err, err_size, "Halide error %d", status);
    info->declined = status == MS_RUN_DECLINE;
    return 0;
}
