// Calling the kernels by name. See engine.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "common/mutex.h"
#include "engine/internal.h"

#define MAX_ARGS 32

// Most threads Halide's CPU kernels use. More are slower on large machines: EdgeAwareBlur
// Soft at 4K on a Linux machine of about 100 threads took 177 ms with 16, 274 with 64 and
// 393 with all of them (the memory, not the cores, limits it).
#define MAX_THREADS 16

int ms_kernel_built(MsKernel k, MsApi api) {
    return (int)k >= 0 && k < MS_KERNEL_COUNT && (int)api >= 0 && api < MS_API_COUNT &&
           ms_kernel_table[k].versions[api].argv != NULL;
}

// The kind of argument a kernel argument takes; -1 for a type the kernels do not use.
static int kind_of(const struct halide_filter_argument_t *a) {
    if (a->kind != halide_argument_kind_input_scalar) // float, or 16-bit floats' bits
        return (a->type.code == halide_type_float || a->type.code == halide_type_uint) &&
                       a->type.bits == 32
                   ? MS_ARG_IMAGE
                   : -1;
    if (a->type.code == halide_type_float)
        return a->type.bits == 32 ? MS_ARG_FLOAT : a->type.bits == 64 ? MS_ARG_DOUBLE : -1;
    return a->type.code == halide_type_int && a->type.bits == 32 ? MS_ARG_INT : -1;
}

// Halide's runtime errors (bounds, GPU driver...): printed, and the call returns an error
// status. Halide's own handler aborts the process, i.e. the host.
static void report_error(void *user_context, const char *message) {
    (void)user_context;
    const int n = (int)strlen(message);
    ms_log("%.*s", n > 0 && message[n - 1] == '\n' ? n - 1 : n, message);
}

// Once: at most MAX_THREADS threads, unless HL_NUM_THREADS sets them (Halide reads it).
// Halide gives its default number only once set: 0 before its first parallel loop.
static void limit_threads(void) {
    static MsMutex lock = MS_MUTEX_INIT;
    static int done;
    ms_mutex_lock(&lock);
    if (!done && !getenv("HL_NUM_THREADS")) {
        halide_set_num_threads(0); // Halide's default: one per processor
        if (halide_get_num_threads() > MAX_THREADS)
            halide_set_num_threads(MAX_THREADS);
    }
    done = 1;
    ms_mutex_unlock(&lock);
}

void ms_catch_errors(void) {
    halide_set_error_handler(report_error);
    limit_threads();
}

static int bad_args(const char *kernel, const char *what, const char *name) {
    ms_log("kernel %s: %s %s", kernel, what, name);
    return MS_KERNEL_BAD_ARGS;
}

int ms_kernel(MsKernel k, MsApi api, const MsArg *args, int count) {
    ms_catch_errors();
    if (!ms_kernel_built(k, api))
        return MS_KERNEL_NOT_BUILT;
    const MsKernelEntry *e = &ms_kernel_table[k];
    const struct halide_filter_metadata_t *m = e->versions[api].metadata();
    if (m->num_arguments > MAX_ARGS || count != m->num_arguments)
        return bad_args(e->name, "wrong number of arguments, given", count > 0 ? args[0].name : "");
    // Halide's argument order is its entry point's; ours can be any.
    void *argv[MAX_ARGS];
    for (int i = 0; i < m->num_arguments; ++i) {
        const struct halide_filter_argument_t *a = &m->arguments[i];
        const MsArg *given = NULL;
        for (int j = 0; j < count && !given; ++j)
            if (strcmp(args[j].name, a->name) == 0)
                given = &args[j];
        if (!given)
            return bad_args(e->name, "missing argument", a->name);
        if ((int)given->kind != kind_of(a))
            return bad_args(e->name, "wrong type for", a->name);
        switch (given->kind) {
        case MS_ARG_IMAGE:
            if (given->image->b.dimensions != a->dimensions)
                return bad_args(e->name, "wrong number of dimensions for", a->name);
            argv[i] = &given->image->b;
            break;
        case MS_ARG_FLOAT:
            argv[i] = (void *)&given->f32; // only read
            break;
        case MS_ARG_DOUBLE:
            argv[i] = (void *)&given->f64;
            break;
        case MS_ARG_INT:
            argv[i] = (void *)&given->i32;
            break;
        }
    }
    return e->versions[api].argv(argv);
}
