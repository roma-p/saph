// OpenGL backend (gl.c): the offscreen context, implemented per platform (gl_mac.c,
// gl_linux.c).

#pragma once

#include <stddef.h>

// Creates an offscreen OpenGL 4.1 core context, current on the calling thread.
// Returns 1, or 0 with the reason in err.
int ms_gl_create_context(char *err, size_t err_size);
void ms_gl_destroy_context(void);

// Address of an OpenGL function, NULL if missing.
void *ms_gl_proc(const char *name);

// How the context was made, e.g. "CGL", "GLX", "EGL".
extern const char *ms_gl_api;
