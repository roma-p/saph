// Running an effect's kernels on the first API that works. See engine.h.

#include <stdio.h>
#include <stdlib.h>

#include "common/clock.h"
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
    const struct halide_device_interface_t *api_interface = ms_device_interface(api);
    if (!api_interface)
        return 0;
    ms_pool_take(&image->b, api_interface);
    if (image->b.device || image->b.host)
        return 0; // with host memory, Halide allocates GPU memory itself when needed
    return halide_device_malloc(NULL, &image->b, api_interface);
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

// A host image in 16-bit floats (ms_image_rgba), converted on `api`: an input to floats
// before the kernels, the output from floats after them. `view`: its 16-bit pixels
// themselves, page-locked for CUDA's copies (`pins`, `pinned` of them, with `context`).
static int convert(MsImage *image, int input, MsApi api, MsImage *view, MsPinned *pins,
                   int *pinned, void *context) {
    ms_image_half(view, image->half, image->half_row, image->dim[0].extent, image->dim[1].extent);
    if (api != MS_API_CPU) {
        if (input)
            view->b.flags |= (uint64_t)halide_buffer_flag_host_dirty;
        ms_pool_take(&view->b, ms_device_interface(api));
        *pinned += ms_pin(&pins[*pinned], &view->b, context);
    }
    if (input)
        return MS_KERNEL(MS_K_TO_FLOAT, api, MS_IMAGE("input", view), MS_IMAGE("output", image));
    const int status =
        MS_KERNEL(MS_K_TO_HALF, api, MS_IMAGE("input", image), MS_IMAGE("output", view));
    return status == 0 && api != MS_API_CPU ? halide_copy_to_host(NULL, &view->b) : status;
}

// One attempt of ms_run on `api` (its arguments, the inputs without repeats). Returns
// Halide's status (MS_RUN_DECLINE: `run` declined the API), and on success the times.
static int attempt(MsRun run, void *user, MsApi api, MsImage *const *inputs, int input_count,
                   MsImage *const *between, int between_count, MsImage *output, MsRunInfo *info) {
    // The host's images (index input_count: the output); those in half get float pixels of
    // their own: on the CPU in host memory, on a GPU in GPU memory (prepare below).
    MsImage *io[MAX_INPUTS + 1];
    float *floats[MAX_INPUTS + 1] = {NULL};
    const int io_count = input_count + 1;
    int status = 0;
    for (int k = 0; k < io_count; ++k) {
        io[k] = k < input_count ? inputs[k] : output;
        if (api != MS_API_CPU || !io[k]->half || status != 0)
            continue;
        floats[k] = malloc((size_t)io[k]->dim[0].extent * io[k]->dim[1].extent * 4 * sizeof(float));
        io[k]->b.host = (uint8_t *)floats[k];
        if (!floats[k])
            status = halide_error_code_out_of_memory;
    }
    // GPU: the images get GPU memory kept from earlier renders where it fits; the inputs are
    // marked as new in host memory (Halide uploads them).
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
    MsPinned pins[2 * (MAX_INPUTS + 1)];
    int pinned = 0;
    void *context = NULL;
    if (status == 0 && api == MS_API_CUDA) {
        if (!output->b.device)
            status = halide_device_malloc(NULL, &output->b, ms_device_interface(api));
        context = status == 0 ? ms_pin_context(&output->b) : NULL;
        for (int k = 0; k < io_count && context; ++k)
            pinned += ms_pin(&pins[pinned], &io[k]->b, context);
    }
    // Timed apart: the kernels with the uploads of the inputs (Halide uploads them when a
    // kernel needs them; half ones: converted first), then the download of the result.
    MsImage views[MAX_INPUTS + 1];
    int viewed[MAX_INPUTS + 1] = {0};
    const double start = ms_clock();
    for (int k = 0; k < input_count && status == 0; ++k)
        if (inputs[k]->half) {
            viewed[k] = 1;
            status = convert(inputs[k], 1, api, &views[k], pins, &pinned, context);
        }
    if (status == 0)
        status = run(api, user);
    if (status == 0 && api != MS_API_CPU)
        status = halide_device_sync(NULL, &output->b);
    const double ran = ms_clock();
    if (status == 0 && output->half) {
        viewed[input_count] = 1;
        status = convert(output, 0, api, &views[input_count], pins, &pinned, context);
    } else if (status == 0) {
        status = halide_copy_to_host(NULL, &output->b);
    }
    *info = (MsRunInfo){api, ran - start, ms_clock() - ran, 0};
    for (int k = 0; k < pinned; ++k)
        ms_unpin(&pins[k], context);
    // The GPU memory goes back to the kept allocations (all freed after a failure).
    const int keep = status == 0 || status == MS_RUN_DECLINE;
    for (int k = 0; k < io_count; ++k) {
        if (viewed[k])
            ms_pool_park(&views[k].b, keep);
        ms_pool_park(&io[k]->b, keep);
        if (floats[k]) {
            io[k]->b.host = NULL;
            free(floats[k]);
        }
    }
    for (int k = 0; k < between_count; ++k)
        ms_pool_park(&between[k]->b, keep);
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
    // The inputs without NULL and repeats (an image given twice is uploaded once).
    MsImage *unique[MAX_INPUTS];
    int unique_count = 0;
    for (int k = 0; k < input_count; ++k) {
        int seen = inputs[k] == NULL;
        for (int j = 0; j < unique_count && !seen; ++j)
            seen = unique[j] == inputs[k];
        if (!seen && unique_count == MAX_INPUTS) {
            snprintf(err, err_size, "internal error: more than %d inputs", MAX_INPUTS);
            return 0;
        }
        if (!seen)
            unique[unique_count++] = inputs[k];
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
        MsRunInfo times;
        status = attempt(run, user, api, unique, unique_count, between, between_count, output,
                         &times);
        if (status == 0) {
            ms_mutex_lock(&lock);
            const int first = !used[api];
            used[api] = 1;
            ms_mutex_unlock(&lock);
            *info = times;
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
