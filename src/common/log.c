// Messages. See log.h.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "common/mutex.h"

static MsMutex lock = MS_MUTEX_INIT;

// MS_LOG_FILE with its folder resolved.
static void log_path(char *path, size_t size) {
#ifdef _WIN32
    const char *temp = getenv("TEMP");
    snprintf(path, size, "%s\\millsaph.log", temp ? temp : ".");
#else
    const char *home = getenv("HOME");
    snprintf(path, size, "%s/millsaph.log", home ? home : "/tmp");
#endif
}

void ms_log(const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    char path[512];
    log_path(path, sizeof path);
    ms_mutex_lock(&lock);
    fprintf(stderr, "[MillSaph] %s\n", line);
    FILE *file = fopen(path, "a"); // none (e.g. another user's file): the terminal only
    if (file) {
        fprintf(file, "[MillSaph] %s\n", line);
        fclose(file);
    }
    ms_mutex_unlock(&lock);
}
