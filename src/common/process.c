// See process.h.

#ifndef _WIN32
#define _GNU_SOURCE // dladdr, mkstemps (glibc)
#endif

#include "common/process.h"

#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

int ms_module_dir(char *out, size_t size) {
    HMODULE module = NULL;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)ms_module_dir, &module) ||
        !GetModuleFileNameA(module, path, sizeof path))
        return 0;
    char *slash = strrchr(path, '\\');
    if (!slash || (size_t)(slash - path + 2) > size)
        return 0;
    slash[1] = 0;
    snprintf(out, size, "%s", path);
    return 1;
}

int ms_temp_file(const char *suffix, char *out, size_t size) {
    char dir[MAX_PATH], name[MAX_PATH];
    if (!GetTempPathA(sizeof dir, dir) || !GetTempFileNameA(dir, "msl", 0, name))
        return 0;
    DeleteFileA(name);
    if ((size_t)snprintf(out, size, "%s%s", name, suffix) >= size)
        return 0;
    FILE *f = fopen(out, "wb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

int ms_run_program(const char *const *argv) {
    // one command line, each argument quoted
    char line[8192] = "";
    size_t used = 0;
    for (int i = 0; argv[i]; ++i) {
        int n = snprintf(line + used, sizeof line - used, "%s\"%s\"", i ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof line - used)
            return -1;
        used += (size_t)n;
    }
    STARTUPINFOA start = {sizeof start};
    PROCESS_INFORMATION process;
    if (!CreateProcessA(argv[0], line, NULL, NULL, FALSE, 0, NULL, NULL, &start, &process))
        return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)code;
}

#else
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int ms_module_dir(char *out, size_t size) {
    Dl_info info;
    if (!dladdr((void *)ms_module_dir, &info) || !info.dli_fname)
        return 0;
    const char *slash = strrchr(info.dli_fname, '/');
    if (!slash || (size_t)(slash - info.dli_fname + 2) > size)
        return 0;
    memcpy(out, info.dli_fname, (size_t)(slash - info.dli_fname + 1));
    out[slash - info.dli_fname + 1] = 0;
    return 1;
}

int ms_temp_file(const char *suffix, char *out, size_t size) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";
    if ((size_t)snprintf(out, size, "%s/millsaph_XXXXXX%s", dir, suffix) >= size)
        return 0;
    const int fd = mkstemps(out, (int)strlen(suffix));
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

int ms_run_program(const char *const *argv) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0)
        return -1;
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif
