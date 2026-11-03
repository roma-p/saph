// A mutex. See mutex.h.

#include "common/mutex.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

_Static_assert(sizeof(MsMutex) == sizeof(SRWLOCK), "MsMutex must hold an SRWLOCK");

void ms_mutex_lock(MsMutex *mutex) { AcquireSRWLockExclusive((PSRWLOCK)mutex); }

void ms_mutex_unlock(MsMutex *mutex) { ReleaseSRWLockExclusive((PSRWLOCK)mutex); }

#else

void ms_mutex_lock(MsMutex *mutex) { pthread_mutex_lock(mutex); }

void ms_mutex_unlock(MsMutex *mutex) { pthread_mutex_unlock(mutex); }

#endif
