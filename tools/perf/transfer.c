// millsaph_bench --transfer [width height]: how fast an RGBA float image goes to the GPU and
// back with CUDA, three ways, to choose how the engine should copy (engine/run.c: Halide
// copies from the host's memory as it is, "pageable"):
//   pageable  the host's memory as it is
//   register  the host's memory page-locked for the copy (cuMemHostRegister), then released
//   staging   copied first into page-locked memory of our own, kept between renders
// Calls the CUDA driver directly (loaded at run time), outside Halide.

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/clock.h"
#include "transfer.h"

#ifdef _WIN32
int ms_transfer_bench(int width, int height) {
    (void)width;
    (void)height;
    printf("--transfer: Linux only\n");
    return 1;
}
#else
#include <dlfcn.h>

typedef int (*CuInit)(unsigned);
typedef int (*CuDeviceGet)(int *, int);
typedef int (*CuCtxRetain)(void **, int);
typedef int (*CuCtxRelease)(int);
typedef int (*CuCtxSet)(void *);
typedef int (*CuMemAlloc)(unsigned long long *, size_t);
typedef int (*CuMemFree)(unsigned long long);
typedef int (*CuHtoD)(unsigned long long, const void *, size_t);
typedef int (*CuDtoH)(void *, unsigned long long, size_t);
typedef int (*CuRegister)(void *, size_t, unsigned);
typedef int (*CuUnregister)(void *);
typedef int (*CuHostAlloc)(void **, size_t, unsigned);
typedef int (*CuHostFree)(void *);

#define TRIES 10

int ms_transfer_bench(int width, int height) {
    void *cuda = dlopen("libcuda.so.1", RTLD_NOW);
    if (!cuda) {
        printf("--transfer: no CUDA driver (libcuda.so.1)\n");
        return 1;
    }
    CuInit init = (CuInit)dlsym(cuda, "cuInit");
    CuDeviceGet device_get = (CuDeviceGet)dlsym(cuda, "cuDeviceGet");
    CuCtxRetain retain = (CuCtxRetain)dlsym(cuda, "cuDevicePrimaryCtxRetain");
    CuCtxRelease release = (CuCtxRelease)dlsym(cuda, "cuDevicePrimaryCtxRelease_v2");
    CuCtxSet set_current = (CuCtxSet)dlsym(cuda, "cuCtxSetCurrent");
    CuMemAlloc mem_alloc = (CuMemAlloc)dlsym(cuda, "cuMemAlloc_v2");
    CuMemFree mem_free = (CuMemFree)dlsym(cuda, "cuMemFree_v2");
    CuHtoD htod = (CuHtoD)dlsym(cuda, "cuMemcpyHtoD_v2");
    CuDtoH dtoh = (CuDtoH)dlsym(cuda, "cuMemcpyDtoH_v2");
    CuRegister host_register = (CuRegister)dlsym(cuda, "cuMemHostRegister_v2");
    CuUnregister host_unregister = (CuUnregister)dlsym(cuda, "cuMemHostUnregister");
    CuHostAlloc host_alloc = (CuHostAlloc)dlsym(cuda, "cuMemHostAlloc");
    CuHostFree host_free = (CuHostFree)dlsym(cuda, "cuMemFreeHost");
    int device = 0;
    void *context = NULL;
    if (!init || !device_get || !retain || !release || !set_current || !mem_alloc ||
        !mem_free || !htod || !dtoh || !host_register || !host_unregister || !host_alloc ||
        !host_free || init(0) != 0 || device_get(&device, 0) != 0 ||
        retain(&context, device) != 0 || set_current(context) != 0) {
        printf("--transfer: CUDA does not start\n");
        dlclose(cuda);
        return 1;
    }

    const size_t bytes = (size_t)width * height * 4 * sizeof(float);
    float *host = malloc(bytes);
    void *pinned = NULL;
    unsigned long long gpu = 0;
    if (!host || host_alloc(&pinned, bytes, 0) != 0 || mem_alloc(&gpu, bytes) != 0) {
        printf("--transfer: not enough memory\n");
        free(host);
        if (pinned)
            host_free(pinned);
        release(device);
        dlclose(cuda);
        return 1;
    }
    for (size_t i = 0; i < bytes / sizeof(float); ++i)
        host[i] = (float)(i % 1000) * 1e-3f;

    // Best of TRIES, after one untimed.
    double up[3] = {1e30, 1e30, 1e30}, down[3] = {1e30, 1e30, 1e30};
    double reg = 1e30, unreg = 1e30, copy_in = 1e30, copy_out = 1e30;
    for (int t = 0; t <= TRIES; ++t) {
        double a = ms_clock();
        htod(gpu, host, bytes);
        double b = ms_clock();
        dtoh(host, gpu, bytes);
        double c = ms_clock();
        if (t > 0) {
            up[0] = b - a < up[0] ? b - a : up[0];
            down[0] = c - b < down[0] ? c - b : down[0];
        }

        a = ms_clock();
        const int registered = host_register(host, bytes, 0) == 0;
        b = ms_clock();
        htod(gpu, host, bytes);
        c = ms_clock();
        dtoh(host, gpu, bytes);
        double d = ms_clock();
        if (registered)
            host_unregister(host);
        double e = ms_clock();
        if (t > 0 && registered) {
            reg = b - a < reg ? b - a : reg;
            up[1] = c - b < up[1] ? c - b : up[1];
            down[1] = d - c < down[1] ? d - c : down[1];
            unreg = e - d < unreg ? e - d : unreg;
        }

        a = ms_clock();
        memcpy(pinned, host, bytes);
        b = ms_clock();
        htod(gpu, pinned, bytes);
        c = ms_clock();
        dtoh(pinned, gpu, bytes);
        d = ms_clock();
        memcpy(host, pinned, bytes);
        e = ms_clock();
        if (t > 0) {
            copy_in = b - a < copy_in ? b - a : copy_in;
            up[2] = c - b < up[2] ? c - b : up[2];
            down[2] = d - c < down[2] ? d - c : down[2];
            copy_out = e - d < copy_out ? e - d : copy_out;
        }
    }

    const double mb = (double)bytes / 1e6;
    printf("CUDA transfers of a %d x %d RGBA float image (%.0f MB), best of %d, ms:\n", width,
           height, mb, TRIES);
    printf("%-10s %10s %10s %10s %10s %10s\n", "way", "prepare", "to GPU", "from GPU", "after",
           "total");
    printf("%-10s %10s %10.2f %10.2f %10s %10.2f   (%.1f GB/s)\n", "pageable", "-", up[0],
           down[0], "-", up[0] + down[0], mb / up[0]);
    if (reg < 1e30)
        printf("%-10s %10.2f %10.2f %10.2f %10.2f %10.2f   (register, copies, unregister)\n",
               "register", reg, up[1], down[1], unreg, reg + up[1] + down[1] + unreg);
    else
        printf("register: cuMemHostRegister failed\n");
    printf("%-10s %10.2f %10.2f %10.2f %10.2f %10.2f   (memcpy in, copies, memcpy out)\n",
           "staging", copy_in, up[2], down[2], copy_out, copy_in + up[2] + down[2] + copy_out);

    mem_free(gpu);
    host_free(pinned);
    free(host);
    release(device);
    dlclose(cuda);
    return 0;
}
#endif
