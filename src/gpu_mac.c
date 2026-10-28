// macOS: offscreen OpenGL context through CGL. A 3.2 core request gives the
// newest core version the GPU supports (4.1 on all recent Macs).

#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <stdio.h>

#include "gpu.h"

const char* ms_gl_api = "CGL";

int ms_gl_create_context(char* err, size_t err_size) {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAAccelerated,
        kCGLPFAAllowOfflineRenderers,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj format;
    GLint count = 0;
    if (CGLChoosePixelFormat(attrs, &format, &count) != kCGLNoError || !format) {
        snprintf(err, err_size, "no OpenGL 4.1 pixel format");
        return 0;
    }
    CGLContextObj context;
    CGLError e = CGLCreateContext(format, NULL, &context);
    CGLDestroyPixelFormat(format);
    if (e != kCGLNoError || CGLSetCurrentContext(context) != kCGLNoError) {
        snprintf(err, err_size, "cannot create an OpenGL context (CGL error %d)", (int)e);
        return 0;
    }
    return 1;
}

void* ms_gl_proc(const char* name) { return dlsym(RTLD_DEFAULT, name); }
