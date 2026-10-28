// Linux: offscreen OpenGL context through EGL, without any X display, so it also
// works on render nodes. libEGL is opened at runtime: nothing GL is linked.
//
// Every EGL GPU device is tried, then the default display; the first that gives a
// current OpenGL 4.1 core context wins. If none does, the error lists each attempt.

#define _GNU_SOURCE  // dladdr
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "gpu.h"

const char* ms_gl_api = "EGL";

static void* egl_lib;
static void* gl_lib;
static PFNEGLGETPROCADDRESSPROC egl_get_proc;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static EGLSurface surface = EGL_NO_SURFACE;

static void* egl_sym(const char* name) {
    void* p = egl_get_proc ? (void*)egl_get_proc(name) : NULL;
    return p ? p : dlsym(egl_lib, name);
}

static struct {
    PFNEGLGETDISPLAYPROC get_display;
    PFNEGLINITIALIZEPROC initialize;
    PFNEGLQUERYSTRINGPROC query_string;
    PFNEGLBINDAPIPROC bind_api;
    PFNEGLCHOOSECONFIGPROC choose_config;
    PFNEGLCREATECONTEXTPROC create_context;
    PFNEGLDESTROYCONTEXTPROC destroy_context;
    PFNEGLCREATEPBUFFERSURFACEPROC create_pbuffer;
    PFNEGLDESTROYSURFACEPROC destroy_surface;
    PFNEGLMAKECURRENTPROC make_current;
    PFNEGLGETERRORPROC get_error;
    PFNEGLQUERYDEVICESEXTPROC query_devices;
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display;
} egl;

// Appends "text" to the attempt log in err.
static void log_attempt(char* err, size_t err_size, const char* text) {
    size_t used = strlen(err);
    if (used < err_size) snprintf(err + used, err_size - used, "%s%s", used ? "; " : "", text);
}

// Tries to get a current OpenGL 4.1 core context on display d. On failure, adds
// "<label> [vendor]: what failed (EGL error)" to err.
static int try_display(EGLDisplay d, const char* label, char* err, size_t err_size) {
    char line[256];
    EGLint major, minor;
    if (d == EGL_NO_DISPLAY) {
        snprintf(line, sizeof line, "%s: no display", label);
        log_attempt(err, err_size, line);
        return 0;
    }
    if (!egl.initialize(d, &major, &minor)) {
        snprintf(line, sizeof line, "%s: eglInitialize failed (0x%04X)", label, (unsigned)egl.get_error());
        log_attempt(err, err_size, line);
        return 0;
    }
    const char* vendor = egl.query_string ? egl.query_string(d, EGL_VENDOR) : NULL;
    char name[128];
    snprintf(name, sizeof name, "%s [%s, EGL %d.%d]", label, vendor ? vendor : "?", (int)major, (int)minor);

    const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
                                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                    EGL_NONE};
    const EGLint pbuffer_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLConfig config;
    EGLint config_count = 0;

    if (!egl.bind_api(EGL_OPENGL_API)) {
        snprintf(line, sizeof line, "%s: no desktop OpenGL (0x%04X)", name, (unsigned)egl.get_error());
    } else if (!egl.choose_config(d, config_attrs, &config, 1, &config_count) || config_count < 1) {
        snprintf(line, sizeof line, "%s: no suitable config (0x%04X)", name, (unsigned)egl.get_error());
    } else {
        EGLContext c = egl.create_context(d, config, EGL_NO_CONTEXT, context_attrs);
        if (c == EGL_NO_CONTEXT) {
            snprintf(line, sizeof line, "%s: no OpenGL 4.1 core context (0x%04X)", name, (unsigned)egl.get_error());
        } else {
            // Surfaceless first; drivers without it need a (tiny, unused) pbuffer.
            if (egl.make_current(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
                display = d;
                context = c;
                return 1;
            }
            const unsigned surfaceless_error = (unsigned)egl.get_error();
            EGLSurface s = egl.create_pbuffer ? egl.create_pbuffer(d, config, pbuffer_attrs) : EGL_NO_SURFACE;
            const unsigned pbuffer_error = (unsigned)egl.get_error();
            if (s != EGL_NO_SURFACE && egl.make_current(d, s, s, c)) {
                display = d;
                context = c;
                surface = s;
                return 1;
            }
            snprintf(line, sizeof line, "%s: eglMakeCurrent failed (surfaceless 0x%04X, pbuffer %s 0x%04X)", name,
                     surfaceless_error, s == EGL_NO_SURFACE ? "creation" : "current",
                     s == EGL_NO_SURFACE ? pbuffer_error : (unsigned)egl.get_error());
            if (s != EGL_NO_SURFACE && egl.destroy_surface) egl.destroy_surface(d, s);
            if (egl.destroy_context) egl.destroy_context(d, c);
        }
    }
    log_attempt(err, err_size, line);
    return 0;
}

int ms_gl_create_context(char* err, size_t err_size) {
    egl_lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!egl_lib) {
        snprintf(err, err_size, "cannot load libEGL.so.1 (is the GPU driver installed?)");
        return 0;
    }
    gl_lib = dlopen("libOpenGL.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!gl_lib) gl_lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    egl_get_proc = (PFNEGLGETPROCADDRESSPROC)dlsym(egl_lib, "eglGetProcAddress");

    egl.get_display = egl_sym("eglGetDisplay");
    egl.initialize = egl_sym("eglInitialize");
    egl.query_string = egl_sym("eglQueryString");
    egl.bind_api = egl_sym("eglBindAPI");
    egl.choose_config = egl_sym("eglChooseConfig");
    egl.create_context = egl_sym("eglCreateContext");
    egl.destroy_context = egl_sym("eglDestroyContext");
    egl.create_pbuffer = egl_sym("eglCreatePbufferSurface");
    egl.destroy_surface = egl_sym("eglDestroySurface");
    egl.make_current = egl_sym("eglMakeCurrent");
    egl.get_error = egl_sym("eglGetError");
    egl.query_devices = egl_sym("eglQueryDevicesEXT");
    egl.platform_display = egl_sym("eglGetPlatformDisplayEXT");
    if (!egl.get_display || !egl.initialize || !egl.bind_api || !egl.choose_config || !egl.create_context ||
        !egl.make_current || !egl.get_error) {
        snprintf(err, err_size, "libEGL is missing core functions");
        return 0;
    }

    // Which libEGL was loaded: hosts can put their own library folders first.
    Dl_info where;
    const char* path = dladdr((void*)egl.initialize, &where) && where.dli_fname ? where.dli_fname : "libEGL.so.1";
    snprintf(err, err_size, "EGL from %s: ", path);
    const size_t prefix = strlen(err);

    // GPU devices directly (no X server needed), then the default display.
    char label[32];
    if (egl.query_devices && egl.platform_display) {
        EGLDeviceEXT devices[16];
        EGLint n = 0;
        if (egl.query_devices(16, devices, &n)) {
            for (EGLint i = 0; i < n; ++i) {
                snprintf(label, sizeof label, "device %d", (int)i);
                if (try_display(egl.platform_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL), label, err + prefix,
                                err_size - prefix))
                    return 1;
            }
        } else {
            log_attempt(err + prefix, err_size - prefix, "no EGL devices listed");
        }
    }
    if (try_display(egl.get_display(EGL_DEFAULT_DISPLAY), "default display", err + prefix, err_size - prefix))
        return 1;
    fprintf(stderr, "[MillSaph] OpenGL setup failed: %s\n", err);
    return 0;
}

void ms_gl_destroy_context(void) {
    if (context == EGL_NO_CONTEXT) return;
    egl.make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE && egl.destroy_surface) egl.destroy_surface(display, surface);
    if (egl.destroy_context) egl.destroy_context(display, context);
    // The display is left initialised: other code in the host process may share it.
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
}

void* ms_gl_proc(const char* name) {
    void* p = egl_get_proc ? (void*)egl_get_proc(name) : NULL;
    return p || !gl_lib ? p : dlsym(gl_lib, name);
}
