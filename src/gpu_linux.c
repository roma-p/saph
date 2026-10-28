// Linux: offscreen OpenGL 4.1 core context. Two ways, tried in this order:
//
// - GLX, when there is an X display ($DISPLAY): what Nuke and Flame themselves use.
//   Inside those hosts an EGL context cannot be made current (their GLX setup and
//   libglvnd refuse it silently), so GLX is required there.
// - EGL, without any display: render nodes, Burn, command-line tools. Every EGL GPU
//   device is tried, then the default display.
//
// Libraries are opened at runtime: nothing GL or X11 is linked, and no X11 headers
// are needed to build. If nothing works, the error lists every attempt.

#define _GNU_SOURCE  // dladdr
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

const char* ms_gl_api = "EGL";  // set to "GLX" when GLX is used

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

// ---------------------------------------------------------------- EGL

static int egl_create(char* err, size_t err_size) {
    egl_lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!egl_lib) {
        snprintf(err, err_size, "EGL: cannot load libEGL.so.1");
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
        snprintf(err, err_size, "EGL: libEGL is missing core functions");
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
    return try_display(egl.get_display(EGL_DEFAULT_DISPLAY), "default display", err + prefix, err_size - prefix);
}

static void egl_destroy(void) {
    if (context == EGL_NO_CONTEXT) return;
    egl.make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE && egl.destroy_surface) egl.destroy_surface(display, surface);
    if (egl.destroy_context) egl.destroy_context(display, context);
    // The display is left initialised: other code in the host process may share it.
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
}

// ---------------------------------------------------------------- GLX
// X11 / GLX types and constants, declared here so no X11 headers are needed.

typedef struct MsXDisplay XDisplay;
typedef struct MsGLXFBConfig* GLXFBConfig;
typedef struct MsGLXContext* GLXContext;
typedef unsigned long XID;
typedef int (*XErrorHandler)(XDisplay*, void*);

#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_RENDER_TYPE 0x8011
#define GLX_PBUFFER_BIT 0x0004
#define GLX_RGBA_BIT 0x0001
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_PBUFFER_WIDTH 0x8041
#define GLX_PBUFFER_HEIGHT 0x8040
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

static struct {
    XDisplay* (*open_display)(const char*);
    int (*close_display)(XDisplay*);
    int (*default_screen)(XDisplay*);
    int (*sync)(XDisplay*, int);
    XErrorHandler (*set_error_handler)(XErrorHandler);
    int (*free)(void*);
    void* (*get_proc)(const unsigned char*);
    GLXFBConfig* (*choose_fb_config)(XDisplay*, int, const int*, int*);
    GLXContext (*create_context)(XDisplay*, GLXFBConfig, GLXContext, int, const int*);
    void (*destroy_context)(XDisplay*, GLXContext);
    XID (*create_pbuffer)(XDisplay*, GLXFBConfig, const int*);
    void (*destroy_pbuffer)(XDisplay*, XID);
    int (*make_current)(XDisplay*, XID, XID, GLXContext);
    // Our own connection to the X server, used only by the GPU thread.
    XDisplay* display;
    GLXContext context;
    XID pbuffer;
} glx;

// X errors are counted, not fatal: the default X error handler would quit the host.
static int x_errors;
static int count_x_error(XDisplay* d, void* event) {
    (void)d;
    (void)event;
    ++x_errors;
    return 0;
}

static int glx_create(const char* display_name, char* err, size_t err_size) {
    void* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    void* libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);  // already loaded by the host
    if (!x11 || !libgl) {
        snprintf(err, err_size, "GLX: cannot load %s", x11 ? "libGL.so.1" : "libX11.so.6");
        return 0;
    }
    *(void**)&glx.open_display = dlsym(x11, "XOpenDisplay");
    *(void**)&glx.close_display = dlsym(x11, "XCloseDisplay");
    *(void**)&glx.default_screen = dlsym(x11, "XDefaultScreen");
    *(void**)&glx.sync = dlsym(x11, "XSync");
    *(void**)&glx.set_error_handler = dlsym(x11, "XSetErrorHandler");
    *(void**)&glx.free = dlsym(x11, "XFree");
    *(void**)&glx.get_proc = dlsym(libgl, "glXGetProcAddressARB");
    *(void**)&glx.choose_fb_config = dlsym(libgl, "glXChooseFBConfig");
    *(void**)&glx.destroy_context = dlsym(libgl, "glXDestroyContext");
    *(void**)&glx.create_pbuffer = dlsym(libgl, "glXCreatePbuffer");
    *(void**)&glx.destroy_pbuffer = dlsym(libgl, "glXDestroyPbuffer");
    *(void**)&glx.make_current = dlsym(libgl, "glXMakeContextCurrent");
    if (glx.get_proc)
        *(void**)&glx.create_context = glx.get_proc((const unsigned char*)"glXCreateContextAttribsARB");
    if (!glx.open_display || !glx.close_display || !glx.default_screen || !glx.sync || !glx.set_error_handler ||
        !glx.free || !glx.get_proc || !glx.choose_fb_config || !glx.create_context || !glx.destroy_context ||
        !glx.create_pbuffer || !glx.destroy_pbuffer || !glx.make_current) {
        snprintf(err, err_size, "GLX: missing functions (glXCreateContextAttribsARB?)");
        return 0;
    }

    glx.display = glx.open_display(display_name);
    if (!glx.display) {
        snprintf(err, err_size, "GLX: cannot open display %s", display_name);
        return 0;
    }
    const int config_attrs[] = {GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                                GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, 0};
    int count = 0;
    GLXFBConfig* configs = glx.choose_fb_config(glx.display, glx.default_screen(glx.display), config_attrs, &count);
    if (!configs || count < 1) {
        snprintf(err, err_size, "GLX on %s: no pbuffer config", display_name);
        glx.close_display(glx.display);
        glx.display = NULL;
        return 0;
    }
    GLXFBConfig config = configs[0];
    glx.free(configs);

    // The handler is process-wide: swap it only around our own calls.
    XErrorHandler previous = glx.set_error_handler(count_x_error);
    x_errors = 0;
    const int context_attrs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 1,
                                 GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    const int pbuffer_attrs[] = {GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, 0};
    const char* failed = NULL;
    glx.context = glx.create_context(glx.display, config, NULL, 1, context_attrs);
    glx.sync(glx.display, 0);
    if (!glx.context || x_errors) {
        failed = "no OpenGL 4.1 core context";
    } else {
        glx.pbuffer = glx.create_pbuffer(glx.display, config, pbuffer_attrs);
        glx.sync(glx.display, 0);
        if (!glx.pbuffer || x_errors) failed = "cannot create a pbuffer";
        else if (!glx.make_current(glx.display, glx.pbuffer, glx.pbuffer, glx.context)) failed = "make current failed";
        glx.sync(glx.display, 0);
        if (!failed && x_errors) failed = "X error while making the context current";
    }
    glx.set_error_handler(previous);
    if (failed) {
        snprintf(err, err_size, "GLX on %s: %s", display_name, failed);
        if (glx.pbuffer) glx.destroy_pbuffer(glx.display, glx.pbuffer);
        if (glx.context) glx.destroy_context(glx.display, glx.context);
        glx.close_display(glx.display);
        glx.display = NULL;
        glx.context = NULL;
        glx.pbuffer = 0;
        return 0;
    }
    ms_gl_api = "GLX";
    return 1;
}

static void glx_destroy(void) {
    if (!glx.display) return;
    glx.make_current(glx.display, 0, 0, NULL);
    glx.destroy_pbuffer(glx.display, glx.pbuffer);
    glx.destroy_context(glx.display, glx.context);
    glx.close_display(glx.display);
    glx.display = NULL;
    glx.context = NULL;
    glx.pbuffer = 0;
}

// ---------------------------------------------------------------- both

int ms_gl_create_context(char* err, size_t err_size) {
    char glx_err[512] = "";
    const char* display_name = getenv("DISPLAY");
    if (display_name && *display_name && glx_create(display_name, glx_err, sizeof glx_err)) return 1;

    ms_gl_api = "EGL";
    char egl_err[2048] = "";
    if (egl_create(egl_err, sizeof egl_err)) return 1;

    snprintf(err, err_size, "%s%s%s", glx_err, *glx_err ? "; " : "", egl_err);
    fprintf(stderr, "[MillSaph] OpenGL setup failed: %s\n", err);
    return 0;
}

void ms_gl_destroy_context(void) {
    glx_destroy();
    egl_destroy();
}

void* ms_gl_proc(const char* name) {
    if (glx.display) return glx.get_proc((const unsigned char*)name);
    void* p = egl_get_proc ? (void*)egl_get_proc(name) : NULL;
    return p || !gl_lib ? p : dlsym(gl_lib, name);
}
