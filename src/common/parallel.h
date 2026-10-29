// Runs a loop over [0, count) on all CPU cores.

#pragma once

// Calls work(begin, end, user) on consecutive slices of [0, count), in parallel, and
// returns when all are done. Returns 0 if threads could not be started (the work is
// then done on the calling thread).
int ms_parallel(int count, void (*work)(int begin, int end, void *user), void *user);
