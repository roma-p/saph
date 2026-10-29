// CPU parallel loop with pthreads. See parallel.h.

#include <pthread.h>
#include <unistd.h>

#include "common/parallel.h"

#define MAX_THREADS 64

typedef struct {
    void (*work)(int begin, int end, void *user);
    void *user;
    int begin, end;
} Slice;

static void *run_slice(void *arg) {
    Slice *s = arg;
    s->work(s->begin, s->end, s->user);
    return NULL;
}

int ms_parallel(int count, void (*work)(int begin, int end, void *user), void *user) {
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    int n = cores < 1 ? 1 : (cores > MAX_THREADS ? MAX_THREADS : (int)cores);
    if (n > count)
        n = count;
    if (n <= 1) {
        if (count > 0)
            work(0, count, user);
        return 1;
    }

    Slice slices[MAX_THREADS];
    pthread_t threads[MAX_THREADS];
    int started[MAX_THREADS] = {0};
    int ok = 1;
    for (int i = 0; i < n; ++i)
        slices[i] = (Slice){work, user, (int)((long long)count * i / n),
                            (int)((long long)count * (i + 1) / n)};
    for (int i = 1; i < n; ++i) {
        started[i] = pthread_create(&threads[i], NULL, run_slice, &slices[i]) == 0;
        if (!started[i]) {
            ok = 0;
            run_slice(&slices[i]); // no thread: do this slice here
        }
    }
    run_slice(&slices[0]);
    for (int i = 1; i < n; ++i)
        if (started[i])
            pthread_join(threads[i], NULL);
    return ok;
}
