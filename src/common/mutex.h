// A mutex: POSIX threads on macOS and Linux, Win32 on Windows.

#pragma once

#ifdef _WIN32
// A Win32 SRWLOCK is one pointer, zero when initialised; kept opaque so that <windows.h>
// stays out of every file that includes this one.
typedef struct {
    void *ptr;
} MsMutex;
#define MS_MUTEX_INIT {0}
#else
#include <pthread.h>
typedef pthread_mutex_t MsMutex;
#define MS_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#endif

void ms_mutex_lock(MsMutex *mutex);
void ms_mutex_unlock(MsMutex *mutex);
