// Running another program from the plugin (the Lens Flare editor): where the plugin is,
// temporary files, a program run and waited for. macOS, Linux, Windows.

#pragma once

#include <stddef.h>

// The folder holding the plugin's binary (the .ofx, or the After Effects plugin), with a
// final separator, into `out`; 0 if unknown.
int ms_module_dir(char *out, size_t size);

// A new temporary file's path ending with `suffix` into `out` (the file created, empty);
// 0 on failure.
int ms_temp_file(const char *suffix, char *out, size_t size);

// Runs program `argv[0]` with `argv` (NULL-terminated) and waits for it to end. Returns its
// exit code, or -1 if it could not run.
int ms_run_program(const char *const *argv);
