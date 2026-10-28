// Linux: offscreen OpenGL context through EGL, without any X display, so it also
// works on render nodes. libEGL is opened at runtime: nothing GL is linked.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>

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

int ms_gl_create_context(char* err, size_t err_size) {
    egl_lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!egl_lib) {
        snprintf(err, err_size, "cannot load libEGL.so.1 (is the GPU driver installed?)");
        return 0;
    }
    gl_lib = dlopen("libOpenGL.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!gl_lib) gl_lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    egl_get_proc = (PFNEGLGETPROCADDRESSPROC)dlsym(egl_lib, "eglGetProcAddress");

    PFNEGLGETDISPLAYPROC get_display = egl_sym("eglGetDisplay");
    PFNEGLINITIALIZEPROC initialize = egl_sym("eglInitialize");
    PFNEGLBINDAPIPROC bind_api = egl_sym("eglBindAPI");
    PFNEGLCHOOSECONFIGPROC choose_config = egl_sym("eglChooseConfig");
    PFNEGLCREATECONTEXTPROC create_context = egl_sym("eglCreateContext");
    PFNEGLCREATEPBUFFERSURFACEPROC create_pbuffer = egl_sym("eglCreatePbufferSurface");
    PFNEGLMAKECURRENTPROC make_current = egl_sym("eglMakeCurrent");
    PFNEGLGETERRORPROC get_error = egl_sym("eglGetError");
    PFNEGLQUERYDEVICESEXTPROC query_devices = egl_sym("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display = egl_sym("eglGetPlatformDisplayEXT");
    if (!get_display || !initialize || !bind_api || !choose_config || !create_context || !make_current) {
        snprintf(err, err_size, "libEGL is missing core functions");
        return 0;
    }

    // Prefer a GPU device directly (no X server needed), then the default display.
    display = EGL_NO_DISPLAY;
    EGLint major, minor;
    if (query_devices && platform_display) {
        EGLDeviceEXT devices[8];
        EGLint n = 0;
        if (query_devices(8, devices, &n)) {
            for (EGLint i = 0; i < n && display == EGL_NO_DISPLAY; ++i) {
                EGLDisplay d = platform_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
                if (d != EGL_NO_DISPLAY && initialize(d, &major, &minor)) display = d;
            }
        }
    }
    if (display == EGL_NO_DISPLAY) {
        EGLDisplay d = get_display(EGL_DEFAULT_DISPLAY);
        if (d != EGL_NO_DISPLAY && initialize(d, &major, &minor)) display = d;
    }
    const char* failed = NULL;
    EGLConfig config;
    EGLint config_count = 0;
    context = EGL_NO_CONTEXT;
    const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
                                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                    EGL_NONE};
    if (display == EGL_NO_DISPLAY) failed = "no EGL display (is the GPU driver installed?)";
    else if (!bind_api(EGL_OPENGL_API)) failed = "EGL cannot use desktop OpenGL";
    else if (!choose_config(display, config_attrs, &config, 1, &config_count) || config_count < 1)
        failed = "no EGL config";
    else if ((context = create_context(display, config, EGL_NO_CONTEXT, context_attrs)) == EGL_NO_CONTEXT)
        failed = "cannot create an OpenGL 4.1 core context";
    else if (!make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        // Drivers without surfaceless contexts need a (tiny, unused) surface.
        const EGLint pbuffer_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = create_pbuffer ? create_pbuffer(display, config, pbuffer_attrs) : EGL_NO_SURFACE;
        if (surface == EGL_NO_SURFACE || !make_current(display, surface, surface, context))
            failed = "eglMakeCurrent failed";
    }
    if (failed) {
        snprintf(err, err_size, "%s (EGL error 0x%04X)", failed, get_error ? (unsigned)get_error() : 0u);
        return 0;
    }
    return 1;
}

void ms_gl_destroy_context(void) {
    if (context == EGL_NO_CONTEXT) return;
    PFNEGLMAKECURRENTPROC make_current = egl_sym("eglMakeCurrent");
    PFNEGLDESTROYCONTEXTPROC destroy_context = egl_sym("eglDestroyContext");
    PFNEGLDESTROYSURFACEPROC destroy_surface = egl_sym("eglDestroySurface");
    if (make_current) make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE && destroy_surface) destroy_surface(display, surface);
    if (destroy_context) destroy_context(display, context);
    // The display is left initialised: other code in the host process may share it.
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
}

void* ms_gl_proc(const char* name) {
    void* p = egl_get_proc ? (void*)egl_get_proc(name) : NULL;
    return p || !gl_lib ? p : dlsym(gl_lib, name);
}
